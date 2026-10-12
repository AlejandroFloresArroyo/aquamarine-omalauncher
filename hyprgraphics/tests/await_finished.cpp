// Regression test for CAsyncResourceGatherer::await(): it must return when the gather thread has
// already finished the resource before await() is called. Hyprland's renderText() enqueues a text
// resource and awaits it right away; when the gather thread wins that race, the original await()
// waited forever (seen as a frozen Hyprland, main thread in await() from the debug overlay).
#include <hyprgraphics/resource/AsyncResourceGatherer.hpp>
#include <hyprgraphics/resource/resources/AsyncResource.hpp>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace Hyprgraphics;
using Hyprutils::Memory::makeAtomicShared;

class CInstantResource : public IAsyncResource {
  public:
    void render() override {}
};

int main() {
    CAsyncResourceGatherer gatherer;
    for (int i = 0; i < 200; ++i) {
        // the gather thread finishes first: what the original code never woke up from
        auto first = makeAtomicShared<CInstantResource>();
        gatherer.enqueue(first);
        while (!first->m_ready)
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        gatherer.await(first);

        // the usual order: await right after enqueue
        auto second = makeAtomicShared<CInstantResource>();
        gatherer.enqueue(second);
        gatherer.await(second);
    }
    std::printf("ok: 400 awaits returned\n");
    return 0;
}
