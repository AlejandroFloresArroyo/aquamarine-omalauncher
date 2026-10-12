// Regression test for CAsyncResourceGatherer::await(): it must return when the gather thread has
// already finished the resource before await() is called. Hyprland's renderText() enqueues a text
// resource and awaits it right away; when the gather thread wins that race, the original await()
// waited forever (seen as a frozen Hyprland, main thread in await() from the debug overlay).
#include <hyprgraphics/resource/AsyncResourceGatherer.hpp>
#include <hyprgraphics/resource/resources/AsyncResource.hpp>
#include <chrono>
#include <cstdio>
#include <thread>
#include <unistd.h>

using namespace Hyprgraphics;
using Hyprutils::Memory::makeAtomicShared;

class CInstantResource : public IAsyncResource {
  public:
    void render() override {}
};

int main() {
    // a regression must fail, not hang: SIGALRM kills the test after 20 s
    alarm(20);

    CAsyncResourceGatherer gatherer;
    for (int i = 0; i < 200; ++i) {
        // the gather thread finishes first: what the original code never woke up from
        auto first = makeAtomicShared<CInstantResource>();
        gatherer.enqueue(first);
        while (!first->m_ready)
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        gatherer.await(first);

        // the usual order: await right after enqueue. With the gather thread's own lost wakeup
        // (needsToProcess set without its mutex) this could stall for its 5 s wait_for timeout.
        auto second = makeAtomicShared<CInstantResource>();
        const auto start = std::chrono::steady_clock::now();
        gatherer.enqueue(second);
        gatherer.await(second);
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(1)) {
            std::printf("FAIL: enqueue+await took over 1 s (lost wakeup of the gather thread)\n");
            return 1;
        }
    }
    std::printf("ok: 400 awaits returned\n");
    return 0;
}
