#include "UiLayout.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <sstream>

namespace {

constexpr int kToolbarCompactWidthDip = 44;
constexpr int kToolbarSmallestWidthDip = 36;
constexpr int kToolbarTopOffsetDip = 96;

struct ToolbarDefinition {
    ToolbarAction action;
    int widthDip;
    int group;
    bool requiredAtNarrowWidths;
};

constexpr std::array kToolbarDefinitions{
    ToolbarDefinition{ToolbarAction::Open, 68, 0, true},
    ToolbarDefinition{ToolbarAction::Back10, 44, 0, false},
    ToolbarDefinition{ToolbarAction::PlayPause, 62, 0, true},
    ToolbarDefinition{ToolbarAction::Stop, 48, 0, false},
    ToolbarDefinition{ToolbarAction::Forward10, 44, 0, false},
    ToolbarDefinition{ToolbarAction::Mute, 54, 0, true},
    ToolbarDefinition{ToolbarAction::ToggleNeuralRendering, 270, 1, true},
    ToolbarDefinition{ToolbarAction::ToggleUpscaling, 230, 1, true},
    ToolbarDefinition{ToolbarAction::ToggleFrameGeneration, 320, 1, true},
    ToolbarDefinition{ToolbarAction::Aspect, 72, 1, false},
    ToolbarDefinition{ToolbarAction::Adjustments, 66, 1, false},
    ToolbarDefinition{ToolbarAction::DebugView, 72, 2, false},
    ToolbarDefinition{ToolbarAction::Fullscreen, 54, 2, true},
};

constexpr size_t RequiredToolbarItemCount()
{
    size_t count = 0;
    for (const auto& definition : kToolbarDefinitions) {
        if (definition.requiredAtNarrowWidths) ++count;
    }
    return count;
}

constexpr size_t kRequiredToolbarItemCount = RequiredToolbarItemCount();

int DipToPixels(int dip, UINT dpi)
{
    return MulDiv(dip, static_cast<int>(dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi),
                  USER_DEFAULT_SCREEN_DPI);
}

int GapPixels(const ToolbarDefinition& previous, const ToolbarDefinition& current, UINT dpi)
{
    return DipToPixels(previous.group == current.group ? kToolbarSpacingDip : kToolbarGroupGapDip, dpi);
}

int LayoutWidth(std::span<const ToolbarDefinition* const> definitions, int itemWidthDip, UINT dpi)
{
    int width = 0;
    for (size_t index = 0; index < definitions.size(); ++index) {
        if (index != 0) width += GapPixels(*definitions[index - 1], *definitions[index], dpi);
        const int widthDip = itemWidthDip == 0 ? definitions[index]->widthDip : itemWidthDip;
        width += DipToPixels(widthDip, dpi);
    }
    return width;
}

std::array<const ToolbarDefinition*, kRequiredToolbarItemCount> RequiredToolbarDefinitions()
{
    std::array<const ToolbarDefinition*, kRequiredToolbarItemCount> required{};
    size_t requiredCount = 0;
    for (const auto& definition : kToolbarDefinitions) {
        if (definition.requiredAtNarrowWidths) required[requiredCount++] = &definition;
    }
    return required;
}

} // namespace

ButtonContentLayout LayoutButtonContent(RECT outer,SIZE icon,SIZE text,bool stacked,UINT dpi)
{
    const int horizontal=std::max(0,DipToPixels(kButtonHorizontalInsetDip,dpi));
    const int vertical=std::max(0,DipToPixels(kButtonVerticalInsetDip,dpi));
    const int gap=std::max(0,DipToPixels(kButtonIconLabelGapDip,dpi));
    RECT inner{std::min<LONG>(outer.right,outer.left+horizontal),
               std::min<LONG>(outer.bottom,outer.top+vertical),
               std::max<LONG>(outer.left,outer.right-horizontal),
               std::max<LONG>(outer.top,outer.bottom-vertical)};
    const int availableWidth=std::max<LONG>(0,inner.right-inner.left);
    const int availableHeight=std::max<LONG>(0,inner.bottom-inner.top);
    const bool hasIcon=icon.cx>0&&icon.cy>0;
    const bool hasText=text.cx>0&&text.cy>0;
    ButtonContentLayout layout{};layout.stacked=stacked&&hasIcon&&hasText;
    const auto centeredRect=[](RECT bounds,int width,int height){
        width=std::clamp(width,0,int(bounds.right-bounds.left));
        height=std::clamp(height,0,int(bounds.bottom-bounds.top));
        const int left=bounds.left+(int(bounds.right-bounds.left)-width)/2;
        const int top=bounds.top+(int(bounds.bottom-bounds.top)-height)/2;
        return RECT{left,top,left+width,top+height};
    };
    if(layout.stacked){
        const int iconHeight=std::min<int>(icon.cy,availableHeight);
        const int textHeight=std::min<int>(text.cy,std::max(0,availableHeight-iconHeight-gap));
        const int totalHeight=iconHeight+(iconHeight&&textHeight?gap:0)+textHeight;
        const int top=inner.top+(availableHeight-totalHeight)/2;
        const int iconWidth=std::min<int>(icon.cx,availableWidth);
        const int textWidth=std::min<int>(text.cx,availableWidth);
        layout.icon=RECT{inner.left+(availableWidth-iconWidth)/2,top,
                         inner.left+(availableWidth+iconWidth)/2,top+iconHeight};
        const int textTop=layout.icon.bottom+(iconHeight&&textHeight?gap:0);
        layout.text=RECT{inner.left+(availableWidth-textWidth)/2,textTop,
                         inner.left+(availableWidth+textWidth)/2,textTop+textHeight};
    }else if(hasIcon&&hasText){
        const int iconWidth=std::min<int>(icon.cx,availableWidth);
        const int actualGap=std::min(gap,std::max(0,availableWidth-iconWidth));
        const int textWidth=std::min<int>(text.cx,std::max(0,availableWidth-iconWidth-actualGap));
        const int totalWidth=iconWidth+actualGap+textWidth;
        const int left=inner.left+(availableWidth-totalWidth)/2;
        layout.icon=RECT{left,inner.top+(availableHeight-std::min<int>(icon.cy,availableHeight))/2,
                         left+iconWidth,inner.top+(availableHeight+std::min<int>(icon.cy,availableHeight))/2};
        layout.text=RECT{layout.icon.right+actualGap,
                         inner.top+(availableHeight-std::min<int>(text.cy,availableHeight))/2,
                         layout.icon.right+actualGap+textWidth,
                         inner.top+(availableHeight+std::min<int>(text.cy,availableHeight))/2};
    }else if(hasIcon){
        layout.icon=centeredRect(inner,icon.cx,icon.cy);layout.text=layout.icon;
    }else{
        layout.text=centeredRect(inner,std::min<int>(text.cx,availableWidth),text.cy);
        layout.icon=layout.text;
    }
    layout.content=RECT{std::min(layout.icon.left,layout.text.left),
                        std::min(layout.icon.top,layout.text.top),
                        std::max(layout.icon.right,layout.text.right),
                        std::max(layout.icon.bottom,layout.text.bottom)};
    return layout;
}

ActivityVisual ResolveActivityVisual(RECT track, uint64_t elapsedMs,
                                     uint64_t completed, uint64_t total,
                                     bool measurable, bool motionEnabled)
{
    ActivityVisual visual{};
    visual.fill = track;
    const LONG width = std::max(0L, track.right-track.left);
    visual.spinnerStep = motionEnabled ? unsigned((elapsedMs/80)%12) : 0;
    visual.indeterminate = !measurable || !total;
    if (!visual.indeterminate) {
        const double fraction = double(std::min(completed,total))/double(total);
        visual.percent = static_cast<unsigned>(fraction*100.0);
        visual.fill.right = track.left+static_cast<LONG>(width*fraction);
    } else {
        const LONG segment = std::min(width,std::max(1L,width/4));
        const double cycle = double(elapsedMs%1600)/800.0;
        const double position = motionEnabled ? (cycle<=1.0?cycle:2.0-cycle) : 0.5;
        visual.fill.left += static_cast<LONG>((width-segment)*position);
        visual.fill.right = visual.fill.left+segment;
    }
    return visual;
}

PreRenderSurfaceLayout LayoutPreRenderSurface(int clientWidth,int clientHeight,UINT dpi)
{
    const int width=std::max(1,clientWidth),height=std::max(1,clientHeight);
    const int gutter=std::min({DipToPixels(24,dpi),width/8,height/10});
    const int left=std::clamp(gutter,0,width),right=std::clamp(width-gutter,left,width);
    const int line=std::max(1,std::min(DipToPixels(24,dpi),height/10));
    const int gap=std::max(1,std::min(DipToPixels(8,dpi),height/30));
    const int buttonWidth=std::min(DipToPixels(120,dpi),std::max(0,right-left));
    const int buttonHeight=std::min(DipToPixels(40,dpi),std::max(1,height-2*gutter));
    const int buttonTop=std::max(gutter,height-gutter-buttonHeight);
    const int progressHeight=std::max(1,std::min(DipToPixels(10,dpi),height/24));
    const int progressBottom=std::max(gutter,buttonTop-gap);
    const int progressTop=std::max(gutter,progressBottom-progressHeight);
    const int textBottom=std::max(gutter,progressTop-gap);
    const int textBlockHeight=std::min(textBottom-gutter,line*6+gap*5);
    const int textTop=std::max(gutter,textBottom-textBlockHeight);
    auto row=[&](int index){
        const int top=std::min(textBottom,textTop+index*(line+gap));
        return RECT{left,top,right,std::min(textBottom,top+line)};
    };
    PreRenderSurfaceLayout layout{};
    layout.title=row(0);layout.phase=row(1);layout.resolution=row(2);
    const int spinnerSide=std::min(DipToPixels(40,dpi),std::max(0,textTop-gutter-gap));
    const int spinnerTop=std::max(0,textTop-gap-spinnerSide);
    layout.spinner=RECT{(width-spinnerSide)/2,spinnerTop,(width+spinnerSide)/2,spinnerTop+spinnerSide};
    layout.frameCount=row(3);layout.elapsedEta=row(4);layout.size=row(5);
    layout.progressTrack=RECT{left,progressTop,right,progressBottom};
    layout.progressFill=layout.progressTrack;layout.progressFill.right=layout.progressFill.left;
    const int buttonLeft=left+(right-left-buttonWidth)/2;
    layout.cancelButton=RECT{buttonLeft,buttonTop,buttonLeft+buttonWidth,
                             std::min(height-gutter,buttonTop+buttonHeight)};
    return layout;
}

IdleSurfaceLayout LayoutIdleSurface(int clientWidth, int clientHeight, UINT dpi)
{
    const int width = std::max(1, clientWidth);
    const int height = std::max(1, clientHeight);
    const int gutter = std::min(DipToPixels(16, dpi), std::max(0, height / 12));
    const int buttonWidth = std::min(DipToPixels(180, dpi), std::max(1, width - 2 * gutter));
    int buttonHeight = DipToPixels(42, dpi);
    int actionGap = DipToPixels(12, dpi);
    int titleHeight = DipToPixels(34, dpi);
    int subtitleHeight = DipToPixels(28, dpi);
    int reasonHeight = DipToPixels(38, dpi);
    int headerGap = DipToPixels(14, dpi);
    int reasonGap = DipToPixels(8, dpi);
    const bool stacked = width < 2 * buttonWidth + actionGap + 2 * gutter;
    int actionBlockHeight = stacked ? buttonHeight * 2 + actionGap : buttonHeight;
    int totalHeight = titleHeight + subtitleHeight + headerGap +
                      actionBlockHeight + reasonGap + reasonHeight;
    const int availableHeight = std::max(1, height - 2 * gutter);
    if (stacked && totalHeight > availableHeight) {
        subtitleHeight = 0;
        const int compactGap = std::min(DipToPixels(4, dpi), availableHeight / 20);
        headerGap = compactGap;
        actionGap = compactGap;
        reasonGap = compactGap;
        buttonHeight = DipToPixels(kToolbarMinHitHeightDip, dpi);
        const int reserved = buttonHeight * 2 + headerGap + actionGap + reasonGap;
        const int textHeight = std::max(0, availableHeight - reserved);
        titleHeight = std::min(DipToPixels(24, dpi), textHeight / 2);
        reasonHeight = std::max(0, textHeight - titleHeight);
        actionBlockHeight = buttonHeight * 2 + actionGap;
        totalHeight = titleHeight + headerGap + actionBlockHeight + reasonGap + reasonHeight;
    }
    const int top = std::max(gutter, (height - totalHeight) / 2);

    IdleSurfaceLayout layout{};
    layout.stacked = stacked;
    layout.title = RECT{gutter, top, width - gutter, top + titleHeight};
    layout.subtitle = RECT{gutter, layout.title.bottom,
                           width - gutter, layout.title.bottom + subtitleHeight};
    const int actionsTop = layout.subtitle.bottom + headerGap;
    if (stacked) {
        const int left = (width - buttonWidth) / 2;
        layout.actions = {
            ToolbarItem{ToolbarAction::Open,
                        RECT{left, actionsTop, left + buttonWidth, actionsTop + buttonHeight}, false},
            ToolbarItem{ToolbarAction::OpenYouTube,
                        RECT{left, actionsTop + buttonHeight + actionGap,
                             left + buttonWidth, actionsTop + buttonHeight * 2 + actionGap}, false},
        };
    } else {
        const int blockWidth = buttonWidth * 2 + actionGap;
        const int left = (width - blockWidth) / 2;
        layout.actions = {
            ToolbarItem{ToolbarAction::Open,
                        RECT{left, actionsTop, left + buttonWidth, actionsTop + buttonHeight}, false},
            ToolbarItem{ToolbarAction::OpenYouTube,
                        RECT{left + buttonWidth + actionGap, actionsTop,
                             left + blockWidth, actionsTop + buttonHeight}, false},
        };
    }
    const int reasonTop = layout.actions[1].bounds.bottom + reasonGap;
    layout.youtubeReason = RECT{gutter, reasonTop, width - gutter,
                                std::min(height, reasonTop + reasonHeight)};
    return layout;
}

std::vector<ToolbarItem> LayoutToolbar(int clientWidth, int clientHeight, UINT dpi)
{
    if (clientWidth <= 0 || clientHeight <= 0) return {};

    std::array<const ToolbarDefinition*, kToolbarDefinitions.size()> all{};
    const auto required = RequiredToolbarDefinitions();
    for (size_t index = 0; index < kToolbarDefinitions.size(); ++index) {
        all[index] = &kToolbarDefinitions[index];
    }

    const int gutter = DipToPixels(kToolbarOuterGutterDip, dpi);
    const int availableWidth = std::max(0, clientWidth - 2 * gutter);
    std::span<const ToolbarDefinition* const> selected{all};
    bool compact = false;
    int itemWidthDip = 0;

    if (LayoutWidth(selected, 0, dpi) > availableWidth) {
        selected = std::span<const ToolbarDefinition* const>{required};
        if (LayoutWidth(selected, 0, dpi) > availableWidth) {
            compact = true;
            itemWidthDip = kToolbarCompactWidthDip;
            if (LayoutWidth(selected, itemWidthDip, dpi) > availableWidth) {
                itemWidthDip = kToolbarSmallestWidthDip;
            }
        }
    }

    const int itemHeight = DipToPixels(kToolbarMinHitHeightDip, dpi);
    const int top = std::max(0, clientHeight - DipToPixels(kToolbarTopOffsetDip, dpi));
    int left = gutter;
    std::vector<ToolbarItem> items;
    items.reserve(selected.size());
    for (size_t index = 0; index < selected.size(); ++index) {
        if (index != 0) left += GapPixels(*selected[index - 1], *selected[index], dpi);
        const int widthDip = itemWidthDip == 0 ? selected[index]->widthDip : itemWidthDip;
        const int width = DipToPixels(widthDip, dpi);
        items.push_back(ToolbarItem{
            selected[index]->action,
            RECT{left, top, left + width, top + itemHeight},
            compact,
        });
        left += width;
    }
    return items;
}

int MinimumToolbarClientWidth(UINT dpi)
{
    const auto required = RequiredToolbarDefinitions();
    const std::span<const ToolbarDefinition* const> selected{required};
    return 2 * DipToPixels(kToolbarOuterGutterDip, dpi) +
           LayoutWidth(selected, 0, dpi);
}

int MinimumIdleClientHeight(UINT dpi)
{
    return DipToPixels(150, dpi);
}

RECT ClampWindowRectToMinimumTrackSize(RECT suggested, POINT minimumTrackSize)
{
    suggested.right = suggested.left +
        std::max<LONG>(suggested.right - suggested.left, minimumTrackSize.x);
    suggested.bottom = suggested.top +
        std::max<LONG>(suggested.bottom - suggested.top, minimumTrackSize.y);
    return suggested;
}

ToolbarAction HitTestToolbar(std::span<const ToolbarItem> items, POINT point)
{
    for (const auto& item : items) {
        if (point.x >= item.bounds.left && point.x < item.bounds.right &&
            point.y >= item.bounds.top && point.y < item.bounds.bottom) {
            return item.action;
        }
    }
    return ToolbarAction::None;
}

std::optional<RECT> LayoutVolumeSlider(int clientWidth, int clientHeight, UINT dpi,
                                      std::span<const ToolbarItem> toolbarItems)
{
    if (clientWidth <= 0 || clientHeight <= 0) return std::nullopt;
    const RECT candidate{
        clientWidth - DipToPixels(185, dpi),
        clientHeight - DipToPixels(69, dpi),
        clientWidth - DipToPixels(95, dpi),
        clientHeight - DipToPixels(61, dpi),
    };
    if (candidate.left < 0 || candidate.top < 0 || candidate.right <= candidate.left ||
        candidate.bottom <= candidate.top) {
        return std::nullopt;
    }
    for (const auto& item : toolbarItems) {
        if (candidate.left < item.bounds.right && candidate.right > item.bounds.left &&
            candidate.top < item.bounds.bottom && candidate.bottom > item.bounds.top) {
            return std::nullopt;
        }
    }
    return candidate;
}

bool IsToolbarActionEnabled(ToolbarAction action, ToolbarAvailability availability)
{
    switch (action) {
    case ToolbarAction::Open:
        return !availability.resolvingYouTube;
    case ToolbarAction::Fullscreen:
        return true;
    case ToolbarAction::OpenYouTube:
        return availability.youtubeAvailable && !availability.resolvingYouTube;
    case ToolbarAction::Back10:
    case ToolbarAction::PlayPause:
    case ToolbarAction::Stop:
    case ToolbarAction::Forward10:
    case ToolbarAction::Mute:
    case ToolbarAction::Aspect:
        return availability.mediaLoaded && !availability.seeking;
    case ToolbarAction::ToggleNeuralRendering:
        return availability.mediaLoaded && !availability.seeking && availability.rendererReady &&
               availability.neuralRenderingAvailable;
    case ToolbarAction::ToggleUpscaling:
        return availability.mediaLoaded && !availability.seeking && availability.rendererReady &&
               availability.upscalingAvailable;
    case ToolbarAction::ToggleFrameGeneration:
        return availability.mediaLoaded && !availability.seeking && availability.rendererReady &&
               availability.frameGenerationAvailable;
    case ToolbarAction::Adjustments:
    case ToolbarAction::DebugView:
        return availability.mediaLoaded && availability.rendererReady;
    case ToolbarAction::None:
        return false;
    }
    return false;
}

std::wstring_view OpenActionLabelKey(bool idleSurface)
{
    return idleSurface ? L"idle.open" : L"button.open";
}

ToolbarAction ReconcileFocusedToolbarAction(std::span<const ToolbarItem> items,
                                            ToolbarAction current,
                                            ToolbarAvailability availability)
{
    for (const auto& item : items) {
        if (item.action == current && IsToolbarActionEnabled(item.action, availability)) {
            return current;
        }
    }
    for (const auto& item : items) {
        if (IsToolbarActionEnabled(item.action, availability)) return item.action;
    }
    return ToolbarAction::None;
}

ToolbarAction NextFocusableToolbarAction(std::span<const ToolbarItem> items,
                                         ToolbarAction current, bool reverse,
                                         ToolbarAvailability availability)
{
    if (items.empty()) return ToolbarAction::None;
    int currentIndex = -1;
    for (size_t index = 0; index < items.size(); ++index) {
        if (items[index].action == current) {
            currentIndex = static_cast<int>(index);
            break;
        }
    }
    for (size_t offset = 0; offset < items.size(); ++offset) {
        currentIndex = reverse
            ? (currentIndex <= 0 ? static_cast<int>(items.size()) - 1 : currentIndex - 1)
            : (currentIndex + 1) % static_cast<int>(items.size());
        const ToolbarAction candidate = items[static_cast<size_t>(currentIndex)].action;
        if (IsToolbarActionEnabled(candidate, availability)) return candidate;
    }
    return ToolbarAction::None;
}

std::vector<RECT> HoverDirtyRectangles(std::span<const ToolbarItem> items,
                                       ToolbarAction oldAction,
                                       ToolbarAction newAction)
{
    if (oldAction == newAction) return {};

    std::vector<RECT> dirty;
    dirty.reserve(2);
    const auto appendBounds = [&](ToolbarAction action) {
        if (action == ToolbarAction::None) return;
        for (const auto& item : items) {
            if (item.action == action) {
                dirty.push_back(item.bounds);
                return;
            }
        }
    };
    appendBounds(oldAction);
    appendBounds(newAction);
    return dirty;
}

ToolbarAction ResolveToolbarHover(std::span<const ToolbarItem> items,
                                  POINT point,
                                  ToolbarAvailability availability)
{
    const ToolbarAction action = HitTestToolbar(items, point);
    return IsToolbarActionEnabled(action, availability) ? action : ToolbarAction::None;
}

ToolbarAction ResolveToolbarHoverForCursor(std::span<const ToolbarItem> items,
                                           std::optional<POINT> clientPoint,
                                           ToolbarAvailability availability)
{
    return clientPoint ? ResolveToolbarHover(items, *clientPoint, availability)
                       : ToolbarAction::None;
}

std::optional<PaintBufferLayout> LayoutPaintBuffer(RECT clientBounds, RECT paintBounds)
{
    const RECT clipped{
        std::max(clientBounds.left, paintBounds.left),
        std::max(clientBounds.top, paintBounds.top),
        std::min(clientBounds.right, paintBounds.right),
        std::min(clientBounds.bottom, paintBounds.bottom),
    };
    if (clipped.right <= clipped.left || clipped.bottom <= clipped.top) return std::nullopt;
    return PaintBufferLayout{
        clipped,
        static_cast<int>(clipped.right - clipped.left),
        static_cast<int>(clipped.bottom - clipped.top),
        POINT{-clipped.left, -clipped.top},
    };
}

std::wstring BuildPlayerStatusText(const PlayerStatusSnapshot& status)
{
    if (status.activity == PlayerStatusActivity::ResolvingYouTube) {
        // Exact user-visible state: Resolving YouTube…
        return L"Resolving YouTube\u2026";
    }
    if (!status.mediaLoaded) return {};

    const wchar_t* configuration = L"Neural addon unavailable";
    if (status.runtimeConfiguration == PlayerRuntimeConfiguration::NeuralAddonExperimental) {
        configuration = L"Neural addon enabled (experimental)";
    } else if (status.runtimeConfiguration == PlayerRuntimeConfiguration::DlssSrSafeMode) {
        configuration = L"DLSS SR safe mode";
    }
    std::wstringstream text;
    text << configuration << L" \u00b7 " << (status.upscalingStatus.empty()?L"DLSS SR unavailable":status.upscalingStatus) << L" \u00b7 FG unavailable"
         << L" \u00b7 Source " << status.sourceWidth << L'\u00d7' << status.sourceHeight
         << L" \u00b7 Input " << status.inputWidth << L'\u00d7' << status.inputHeight
         << L" \u00b7 Output " << status.outputWidth << L'\u00d7' << status.outputHeight
         << L" \u00b7 " << (status.quality.empty() ? L"\u2014" : status.quality)
         << L" \u00b7 FPS " << static_cast<int>(std::lround(status.renderedFps))
         << L" rendered / " << static_cast<int>(std::lround(status.sourceFps))
         << L" source \u00b7 Dropped " << status.droppedFrames;
    return text.str();
}

std::wstring BuildPlayerWindowTitle(std::wstring_view appTitle,
                                    std::wstring_view mediaTitle,
                                    size_t maxCharacters)
{
    if (mediaTitle.empty()) return std::wstring(appTitle);
    const std::wstring prefix = std::wstring(appTitle) + L" \u2014 ";
    const std::wstring complete = prefix + std::wstring(mediaTitle);
    if (maxCharacters == 0 || complete.size() <= maxCharacters) return complete;
    if (prefix.size() >= maxCharacters) {
        if (maxCharacters == 1) return L"\u2026";
        return std::wstring(appTitle.substr(0, maxCharacters - 1)) + L"\u2026";
    }
    const size_t mediaCharacters = maxCharacters - prefix.size();
    if (mediaCharacters == 1) return prefix + L"\u2026";
    return prefix + std::wstring(mediaTitle.substr(0, mediaCharacters - 1)) + L"\u2026";
}

PlayerRuntimeStatus ResolvePlayerRuntimeStatus(bool safeMode,
                                               bool neuralAddonConfigured,
                                               bool dlssEnabled,
                                               bool dlssFeatureCreated)
{
    PlayerRuntimeStatus status{};
    if (safeMode) {
        status.configuration = PlayerRuntimeConfiguration::DlssSrSafeMode;
    } else if (neuralAddonConfigured) {
        status.configuration = PlayerRuntimeConfiguration::NeuralAddonExperimental;
    }
    status.dlssState = dlssEnabled && dlssFeatureCreated
        ? PlayerDlssState::Active : PlayerDlssState::ScalerFallback;
    return status;
}

bool ExecuteGuardedRehook(int dialogResult, const std::function<void()>& requestRecreate)
{
    if (dialogResult != IDYES || !requestRecreate) return false;
    requestRecreate();
    return true;
}

uint64_t YouTubeResolutionLifecycle::Begin()
{
    ++generation_;
    resolving_ = true;
    return generation_;
}

bool YouTubeResolutionLifecycle::Complete(uint64_t generation)
{
    if (!resolving_ || generation != generation_) return false;
    resolving_ = false;
    return true;
}

void YouTubeResolutionLifecycle::Invalidate()
{
    ++generation_;
    resolving_ = false;
}

DecoderOpenPolicy DecoderPolicyForSource(MediaSourceKind sourceKind)
{
    return sourceKind == MediaSourceKind::YouTube
        ? DecoderOpenPolicy::FfmpegOnly
        : DecoderOpenPolicy::FfmpegThenMediaFoundation;
}

std::wstring DisplayTitleForSource(MediaSourceKind sourceKind,
                                   std::wstring_view suppliedTitle)
{
    if (sourceKind != MediaSourceKind::YouTube) return std::wstring(suppliedTitle);
    if (suppliedTitle.empty() || suppliedTitle.find(L"://") != std::wstring_view::npos ||
        suppliedTitle.find(L"googlevideo") != std::wstring_view::npos) {
        return L"YouTube video";
    }
    return std::wstring(suppliedTitle);
}

std::string_view SafeSourceLogLabel(MediaSourceKind sourceKind)
{
    return sourceKind == MediaSourceKind::YouTube ? "YouTube stream" : "local file";
}

void ExecuteYouTubeCancellationSequence(const std::function<void()>& requestStop,
                                        const std::function<void()>& cancelResolver,
                                        const std::function<void()>& joinWorker)
{
    if (requestStop) requestStop();
    if (cancelResolver) cancelResolver();
    if (joinWorker) joinWorker();
}

bool YouTubePlaybackAvailable()
{
    return true;
}
