param([Parameter(Mandatory=$true)][string]$Process,[Parameter(Mandatory=$true)][string]$Out)
# Captures one window by handle rather than the screen: the player is routinely
# occluded on a developer desktop, and SetForegroundWindow is refused for a
# process the shell did not activate, which silently produced screenshots of
# whatever was on top instead.
Add-Type -AssemblyName System.Drawing
Add-Type -Namespace Win -Name Cap -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
public struct RECT { public int Left, Top, Right, Bottom; }
'@
$p = Get-Process -Name $Process -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { throw "no window for $Process" }
$rect = New-Object Win.Cap+RECT
[void][Win.Cap]::GetClientRect($p.MainWindowHandle, [ref]$rect)
$bmp = New-Object System.Drawing.Bitmap ($rect.Right - $rect.Left), ($rect.Bottom - $rect.Top)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
# PW_RENDERFULLCONTENT (2): required for a swapchain-backed child window.
[void][Win.Cap]::PrintWindow($p.MainWindowHandle, $dc, 2)
$g.ReleaseHdc($dc)
$bmp.Save($Out)
"$($bmp.Width)x$($bmp.Height) -> $Out"
