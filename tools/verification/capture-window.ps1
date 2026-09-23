[CmdletBinding(DefaultParameterSetName='Capture')]
param([Parameter(Mandatory=$true,ParameterSetName='Capture')][string]$Process,
      [Parameter(Mandatory=$true,ParameterSetName='Capture')][string]$Out,
      [Parameter(Mandatory=$true,ParameterSetName='SelfTest')][switch]$SelfTest)
# Captures one window by handle rather than the screen: the player is routinely
# occluded on a developer desktop, and SetForegroundWindow is refused for a
# process the shell did not activate, which silently produced screenshots of
# whatever was on top instead.
#
# -SelfTest paints a window of its own with the colour GDI+ keys on (below) and
# its neighbours, captures it through the same function, and fails unless every
# one comes back exactly as drawn.
Add-Type -AssemblyName System.Drawing
Add-Type -Namespace Win -Name Cap -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr h);
[DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr h, IntPtr dc);
[DllImport("gdi32.dll")] public static extern IntPtr CreateCompatibleDC(IntPtr dc);
[DllImport("gdi32.dll")] public static extern IntPtr CreateCompatibleBitmap(IntPtr dc, int w, int h);
[DllImport("gdi32.dll")] public static extern IntPtr SelectObject(IntPtr dc, IntPtr o);
[DllImport("gdi32.dll")] public static extern bool DeleteObject(IntPtr o);
[DllImport("gdi32.dll")] public static extern bool DeleteDC(IntPtr dc);
public struct RECT { public int Left, Top, Right, Bottom; }
'@
# Physical pixels, not the scaled ones a DPI-unaware PowerShell is given: at
# 150% GetWindowRect reported two thirds of the player's real size and the
# bitmap kept only its top-left corner.
[void][Win.Cap]::SetProcessDPIAware()

# PrintWindow draws into a GDI bitmap of our own, which becomes a System.Drawing
# image only afterwards. It used to draw into the HDC of Graphics.FromImage(bitmap)
# - and GDI+ hands out that HDC pre-filled with RGB(13,11,12), 0x0D0B0C, and on
# ReleaseHdc takes every pixel still holding that exact value as "not drawn": it
# comes back as (0,0,0), with alpha 0 in the default 32bppArgb bitmap. Measured on
# this machine for 32bppArgb, 32bppRgb and 24bppRgb alike; a FillRect of
# (13,11,11) or (12,11,12) survives, (13,11,12) does not. That is sRGB 13,11,12,
# a near-black a dark video really produces: a neural frame of the Mafia trailer
# had 70,752 pixels at exactly YUV (26,128,129), which decodes to it, and the
# player's screenshot showed 10,699 of them as pure-black "crushed" patches that
# the player never drew. Every one had alpha 0; not one pixel of that colour
# survived in any capture, while all its neighbours did.
function Capture-Window([IntPtr]$Handle) {
    # GetWindowRect, not GetClientRect: PrintWindow renders the whole window into
    # the target DC starting at the frame's top-left, so a bitmap sized to the
    # client silently cut the bottom of the window off. That clipped band is where
    # this player draws its status line and its entire seek bar, so a screenshot
    # taken this way "proved" both were missing when both were being drawn.
    $rect = New-Object Win.Cap+RECT
    [void][Win.Cap]::GetWindowRect($Handle, [ref]$rect)
    $w = $rect.Right - $rect.Left; $h = $rect.Bottom - $rect.Top
    $screen = [Win.Cap]::GetDC([IntPtr]::Zero)
    $dc = [Win.Cap]::CreateCompatibleDC($screen)
    $hbm = [Win.Cap]::CreateCompatibleBitmap($screen, $w, $h)
    $old = [Win.Cap]::SelectObject($dc, $hbm)
    try {
        # PW_RENDERFULLCONTENT (2): required for a swapchain-backed child window.
        [void][Win.Cap]::PrintWindow($Handle, $dc, 2)
        [void][Win.Cap]::SelectObject($dc, $old)
        # FromHbitmap copies the bits as they are (32bppRgb, opaque): no key.
        [System.Drawing.Image]::FromHbitmap($hbm)
    } finally {
        [void][Win.Cap]::DeleteObject($hbm); [void][Win.Cap]::DeleteDC($dc)
        [void][Win.Cap]::ReleaseDC([IntPtr]::Zero, $screen)
    }
}

if ($SelfTest) {
    Add-Type -AssemblyName System.Windows.Forms
    $colours = @(@(13,11,12), @(13,11,11), @(12,11,12), @(14,11,12), @(0,0,0), @(255,255,255))
    $form = New-Object System.Windows.Forms.Form
    $form.FormBorderStyle = 'None'; $form.StartPosition = 'Manual'; $form.ShowInTaskbar = $false
    $form.Location = New-Object System.Drawing.Point(0, 0); $form.ClientSize = New-Object System.Drawing.Size(($colours.Count * 32), 32)
    $form.Add_Paint({ param($s, $e)
        for ($i = 0; $i -lt $colours.Count; $i++) {
            $c = $colours[$i]; $b = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb($c[0], $c[1], $c[2]))
            $e.Graphics.FillRectangle($b, $i * 32, 0, 32, 32); $b.Dispose()
        }
    })
    $form.Show(); [System.Windows.Forms.Application]::DoEvents(); Start-Sleep -Milliseconds 300; [System.Windows.Forms.Application]::DoEvents()
    $image = Capture-Window $form.Handle
    $form.Close(); $form.Dispose()
    $bad = 0
    for ($i = 0; $i -lt $colours.Count; $i++) {
        $c = $colours[$i]; $p = $image.GetPixel($i * 32 + 16, 16)
        $ok = $p.A -eq 255 -and $p.R -eq $c[0] -and $p.G -eq $c[1] -and $p.B -eq $c[2]
        if (-not $ok) { ++$bad }
        "drew ($($c -join ',')) captured ($($p.R),$($p.G),$($p.B)) alpha $($p.A) $(if ($ok) { 'ok' } else { 'WRONG' })"
    }
    $image.Dispose()
    if ($bad) { throw "capture changed $bad of $($colours.Count) colours" }
    'self-test passed'
    return
}

$p = Get-Process -Name $Process -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { throw "no window for $Process" }
$bmp = Capture-Window $p.MainWindowHandle
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
"$($bmp.Width)x$($bmp.Height) -> $Out"
$bmp.Dispose()
