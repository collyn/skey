#include "sheets_cell_tracker.h"
#include "vietnamese.h"
#include <cstdlib>
#include <iostream>

static void check(bool value, const char *message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

int main() {
    SheetsCellTracker tracker;
    tracker.focus(":1.28", "/editor", true, "G27");
    check(!tracker.update(":1.28", "/editor", "G27"), "same cell reset");
    check(!tracker.update(":1.28", "/editor", ""), "empty name reset");
    check(!tracker.update(":1.28", "/editor", "gõ"), "typing reset");
    check(!tracker.update(":1.29", "/editor", "H27"), "other app reset");
    check(!tracker.update(":1.28", "/background", "H27"), "background reset");
    check(tracker.update(":1.28", "/editor", "H27"), "mouse cell change missed");
    check(!tracker.update(":1.28", "/editor", "H27"), "duplicate reset");
    check(tracker.update(":1.28", "/editor", "G28"), "second click missed");
    tracker.focus(":1.28", "/editor", true, "G28");
    check(tracker.update(":1.28", "/editor", "R24"), "repeat focus lost tracking");
    tracker.focus(":1.28", "/chat", false, "");
    check(!tracker.update(":1.28", "/editor", "R25"), "unfocused Sheets reset");
    tracker.focus(":1.28", "/editor", true, "");
    check(!tracker.update(":1.28", "/editor", "R25"), "initial name reset");
    check(!tracker.update(":1.28", "/editor", "A0"), "invalid row reset");
    check(!tracker.update(":1.28", "/editor", "ABCD1"), "invalid column reset");
    check(tracker.update(":1.28", "/editor", "R26"), "change after empty seed missed");

    // Regression: the new cell's async event arrives only after 'go'. The
    // authoritative read must reset BEFORE 'g', and the later event must not
    // reset again before 'x'. All three cells retain the full raw word.
    SheetsCellSnapshot snapshot;
    SheetsCellTracker delayedEvents;
    delayedEvents.focus(":1.28", "/editor", true, "F24");
    skey::VietnameseEngine viet;
    viet.setMethod(skey::InputMethod::Telex);
    for (const char *cell : {"F24", "G24", "H24"}) {
        for (char key : std::string("gox")) {
            if (key == 'x') delayedEvents.update(":1.28", "/editor", cell);
            if (snapshot.observe(":1.28/editor", cell)) viet.reset();
            viet.processKey(key);
        }
        check(viet.getRawInput() == "gox", "late event split the word");
        check(viet.getComposed() == "gõ", "composition leaked across cells");
    }
    check(!snapshot.observe(":1.28/editor", ""), "failed read reset baseline");
    check(!snapshot.observe(":1.28/editor", "H24"), "retry reset same cell");
    check(snapshot.observe(":1.52/editor", "H24"), "new editor identity missed");

    // A deferred commit may run after a click, before the new cell's first
    // key. Its synchronous read must invalidate the old composition now,
    // while the subsequent key observes the same baseline without resetting.
    check(snapshot.observe(":1.52/editor", "I24"), "commit boundary missed cell switch");
    viet.reset();
    check(!snapshot.observe(":1.52/editor", "I24"), "first key reset after commit boundary");
    for (char key : std::string("chaof")) viet.processKey(key);
    check(viet.getComposed() == "chào", "first word after pending commit was corrupted");
    for (const char *invalid : {"", "loading", "A0", "gõ"})
        check(!snapshot.observe(":1.52/editor", invalid), "invalid read advanced cell baseline");
    check(snapshot.observe(":1.52/editor", "J24"), "recovery after failed reads missed switch");
    check(snapshot.observe(":1.52/other-editor", "J24"), "same coordinate in another document missed");
    check(!snapshot.observe(":1.52/other-editor", "J24"), "repeat observation reset word");
    std::cout << "Sheets cell tracking passed\n";
}
