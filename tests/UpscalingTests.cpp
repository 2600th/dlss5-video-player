#include "UpscalingPolicy.h"
#include "NgxSession.h"
#include "TestSupport.h"

int main() {
    const auto hd = UpscalingTarget(1920,1080,1440);
    CHECK_EQ(hd.width,2560u); CHECK_EQ(hd.height,1440u); CHECK(hd.grows);
    const auto uhd = UpscalingTarget(1920,1080,2160);
    CHECK_EQ(uhd.width,3840u); CHECK_EQ(uhd.height,2160u); CHECK(uhd.grows);
    const auto native = UpscalingTarget(3840,2160,1440);
    CHECK_EQ(native.width,3840u); CHECK_EQ(native.height,2160u); CHECK(!native.grows);
    const auto wide = UpscalingTarget(1920,800,1440);
    CHECK_EQ(wide.width,2560u); CHECK_EQ(wide.height,1066u); CHECK(wide.grows);
    CHECK(!UpscalingTarget(0,1080,1440).grows);
    CHECK(!UpscalingTarget(1920,1080,720).grows);
    CHECK(SourceFitsDLSSRange(1920,1080,2560,1440,1280,720,2560,1440));
    CHECK(!SourceFitsDLSSRange(1920,1080,2560,1440,1280,720,1706,960));
    CHECK(!SourceFitsDLSSRange(3840,2160,2560,1440,1,1,3840,2160));
    CHECK(!SourceFitsDLSSRange(1920,1080,2560,1440,0,0,0,0));
    bool delayed=true,recreate=false,created=false;
    const auto first=ngx_session_detail::PrepareFeatureForFrame(true,false,1,delayed,recreate,
        [&]{created=true;return true;},[]{return false;},true);
    CHECK(created);CHECK(first.selected);CHECK(first.needsFlush);
    return test_support::failure_count==0?0:1;
}
