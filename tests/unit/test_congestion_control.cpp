#include "proxy/monitor/CongestionControl.h"

#include <cassert>

using proxy::monitor::CongestionControl;

int main() {
    CongestionControl::Config cfg;
    cfg.enabled = true;
    cfg.initialWindow = 4;
    cfg.minWindow = 1;
    cfg.maxWindow = 10;
    cfg.additiveIncrease = 1;
    cfg.multiplicativeDecrease = 0.5;
    CongestionControl cc(cfg);

    // Acquire up to cwnd.
    assert(cc.TryAcquire());
    assert(cc.TryAcquire());
    assert(cc.TryAcquire());
    assert(cc.TryAcquire());
    assert(!cc.TryAcquire());

    // Complete 4 successes -> should increase cwnd by 1 (ackCounter >= cwnd).
    cc.OnComplete(true);
    cc.OnComplete(true);
    cc.OnComplete(true);
    cc.OnComplete(true);
    auto st = cc.GetStats();
    assert(st.cwnd == 5);
    assert(st.inflight == 0);

    // One loss -> cwnd halves (floor) but not below min.
    assert(cc.TryAcquire());
    cc.OnComplete(false);
    st = cc.GetStats();
    assert(st.cwnd == 2);
    assert(st.inflight == 0);

    // Drop without acquire should also decrease.
    cc.OnDrop();
    st = cc.GetStats();
    assert(st.cwnd == 1);

    // Min window respected.
    cc.OnDrop();
    st = cc.GetStats();
    assert(st.cwnd == 1);
    return 0;
}

