#ifndef SKEY_MODE_CHOICES_H
#define SKEY_MODE_CHOICES_H
#include <QComboBox>
#include <QSignalBlocker>
#include "tr.h"

// Keep serialized values stable while swapping the transport shown at slot 2.
inline void syncTransportChoices(QComboBox *combo, bool replace) {
    QString selected = combo->currentData().toString();
    const QSignalBlocker blocker(combo);
    for (const auto &mode : {QStringLiteral("Uinput"), QStringLiteral("Native")}) {
        int index = combo->findData(mode);
        if (index >= 0) combo->removeItem(index);
    }
    combo->insertItem(1, replace ? T("Native") : T("Uinput"),
                      replace ? "Native" : "Uinput");
    if (!replace) combo->insertItem(4, T("Native (XTest/Libei)"), "Native");
    if (replace && selected == QLatin1String("Uinput")) selected = "Native";
    const int index = combo->findData(selected);
    if (index >= 0) combo->setCurrentIndex(index);
}
#endif
