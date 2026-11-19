#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "stubs/Preferences.h"
#include "../AuctionDevice/BidJournal.h"

void check(bool ok, const char* description) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", description); std::exit(1); }
    std::printf("PASS: %s\n", description);
}

int main() {
    constexpr char id[] = "11111111-1111-4111-8111-111111111111";
    BidJournal journal;
    check(!bidNamespaceExists && journal.load(id) && bidNamespaceExists && !journal.hasPending(),
          "first boot creates an empty bid namespace without a bid");
    BidJournal::Entry bid{};
    std::strcpy(bid.deviceId, id);
    std::strcpy(bid.requestId, "22222222-2222-4222-8222-222222222222");
    std::strcpy(bid.sessionId, "33333333-3333-4333-8333-333333333333");
    std::strcpy(bid.auctionId, "44444444-4444-4444-8444-444444444444");
    std::strcpy(bid.lotId, "55555555-5555-4555-8555-555555555555");
    std::strcpy(bid.payload, "{\"type\":\"placeBid\",\"requestId\":\"22222222-2222-4222-8222-222222222222\"}");
    bid.sessionRevision = 7; bid.amountOre = 12300; bid.outcome = 1;
    nvsWriteLimit = 100;
    check(!journal.save(bid) && std::strcmp(journal.lastWriteFailure(), "JOURNAL_PUT") == 0,
          "partial write prevents first send and reports storage failure");
    check(!journal.save(bid) && std::strcmp(journal.lastWriteFailure(), "JOURNAL_PUT") == 0,
          "repeated write preserves first storage failure");
    BidJournal restart;
    check(!restart.load(id), "corrupt only slot blocks bidding");
    bidBlob0.clear(); bidBlob1.clear(); nvsWriteLimit = SIZE_MAX;
    check(restart.load(id) && restart.save(bid), "complete payload saved before send");
    BidJournal afterPowerLoss;
    check(afterPowerLoss.load(id) && afterPowerLoss.hasPending() &&
          std::strcmp(afterPowerLoss.entry().payload, bid.payload) == 0 &&
          std::strcmp(afterPowerLoss.entry().sessionId, bid.sessionId) == 0 &&
          afterPowerLoss.entry().amountOre == 12300, "restart recovers exact request and session");
    nvsWriteLimit = 100;
    check(!afterPowerLoss.clear(), "interrupted clear does not permit new bid");
    BidJournal afterInterruptedClear;
    check(afterInterruptedClear.load(id) && afterInterruptedClear.hasPending(),
          "old complete request survives interrupted clear");
    nvsWriteLimit = SIZE_MAX;
    check(afterInterruptedClear.clear(), "acknowledged final request clears durably");
    BidJournal afterClear;
    check(afterClear.load(id) && !afterClear.hasPending(), "cleared journal stays clear after restart");
    check(!afterClear.load("66666666-6666-4666-8666-666666666666"), "different device identity cannot adopt journal");
    check(afterClear.load(id) && afterClear.save(bid), "new request follows a durable clear");
    bidBlob0[50] ^= 1;
    BidJournal damagedRequest;
    check(!damagedRequest.load(id), "corrupt newer request cannot fall back to old clear marker");
}
