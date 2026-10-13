// UI texts in English and Dutch (Settings -> Language). One table: every line
// holds an id with its English and Dutch text, so the two can't drift apart.
// UTF-8, written as escapes; the UI fonts cover codepage 1252.
#pragma once

#include <stdint.h>

enum class Lang : uint8_t { En, Nl, Count };

#define T5_STRINGS(X)                                                                      \
  /* main menu */                                                                          \
  X(Resume, "Resume", "Verder lezen")                                                      \
  X(Chapters, "Chapters", "Hoofdstukken")                                                  \
  X(Bookmarks, "Bookmarks", "Bladwijzers")                                                 \
  X(GoTo, "Go to position", "Ga naar positie")                                             \
  X(Library, "Library", "Bibliotheek")                                                     \
  X(Statistics, "Statistics", "Statistieken")                                              \
  X(WifiTransfer, "Wi-Fi transfer", "Wifi-overdracht")                                     \
  X(Settings, "Settings", "Instellingen")                                                  \
  X(About, "About", "Over")                                                                \
  X(Lock, "Lock", "Vergrendelen")                                                          \
  X(Sleep, "Sleep", "Slapen")                                                              \
  /* footer actions */                                                                     \
  X(Open, "open", "open")                                                                  \
  X(Back, "back", "terug")                                                                 \
  X(Go, "go", "ga")                                                                        \
  X(Edit, "edit", "wijzig")                                                                \
  X(ShowPage, "show page", "toon pagina")                                                  \
  X(ShowWifi, "show Wi-Fi", "toon wifi")                                                   \
  X(Done, "done", "klaar")                                                                 \
  X(Retry, "retry", "opnieuw")                                                             \
  /* messages */                                                                           \
  X(CannotOpenBook, "Cannot open book", "Kan boek niet openen")                            \
  X(ReadError, "Read error", "Leesfout")                                                   \
  X(CardRemoved, "Was the card removed?", "Is de kaart eruit gehaald?")                    \
  X(NoBooksYet, "No books yet", "Nog geen boeken")                                         \
  X(NoBooksLine1, "Upload them over Wi-Fi, or copy", "Stuur ze via wifi, of kopieer")      \
  X(NoBooksLine2, ".t5b files to /books on the card", ".t5b-bestanden naar /books")        \
  X(NoCard, "No SD card", "Geen SD-kaart")                                                 \
  X(NoCardLine1, "Insert a FAT32 card", "Plaats een FAT32-kaart")                          \
  X(NoCardLine2, "with books in /books", "met boeken in /books")                           \
  X(BatteryLow, "Battery low", "Batterij bijna leeg")                                      \
  X(BatteryLine1, "Charge the reader,", "Laad de lezer op")                                \
  X(BatteryLine2, "then press a button", "en druk dan op een knop")                        \
  X(RetryAnyButton, "Press any button to try again", "Druk op een knop om het opnieuw te proberen") \
  X(RetryButton, "Press the button to try again", "Druk op de knop om het opnieuw te proberen") \
  X(RetryTap, "Tap the screen to try again", "Tik op het scherm om het opnieuw te proberen") \
  X(SleepAnyButton, "Sleeping  \xC2\xB7  press any button", "Slaapt  \xC2\xB7  druk op een knop") \
  X(SleepButton, "Sleeping  \xC2\xB7  press the button", "Slaapt  \xC2\xB7  druk op de knop") \
  X(OffPowerButton, "Off  \xC2\xB7  press the power button to read on",                     \
    "Uit  \xC2\xB7  aan/uit-knop om verder te lezen")                                      \
  X(OffSwitch, "Off  \xC2\xB7  press the switch to read on",                                \
    "Uit  \xC2\xB7  druk de schakelaar in om verder te lezen")                             \
  X(NothingHere, "Nothing here", "Niets te zien")                                          \
  X(PageOf, "Page %u of %u", "Pagina %u van %u")                                           \
  X(PageN, "Page %u", "Pagina %u")                                                         \
  X(New, "new", "nieuw")                                                                   \
  X(BooksN, "%u books", "%u boeken")                                                       \
  /* about */                                                                              \
  X(Firmware, "Firmware", "Firmware")                                                      \
  X(Battery, "Battery", "Batterij")                                                        \
  X(Book, "Book", "Boek")                                                                  \
  X(Position, "Position", "Positie")                                                       \
  X(Memory, "Memory", "Geheugen")                                                          \
  X(WakeToPage, "Wake to page", "Wekken tot pagina")                                       \
  X(UsbPower, "USB power", "USB-voeding")                                                  \
  X(PositionValue, "page %u/%u  \xC2\xB7  ch. %d/%d", "pagina %u/%u  \xC2\xB7  hfst. %d/%d") \
  X(MemoryValue, "%u MB card  \xC2\xB7  %u KB free", "%u MB kaart  \xC2\xB7  %u KB vrij")  \
  X(WakeValue, "%u ms  \xC2\xB7  %u wakes", "%u ms  \xC2\xB7  %u keer gewekt")             \
  /* Wi-Fi transfer */                                                                     \
  X(JoinWifi, "Join the Wi-Fi", "Verbind met de wifi")                                     \
  X(Network, "Network", "Netwerk")                                                         \
  X(Password, "Password", "Wachtwoord")                                                    \
  X(OpenThePage, "Open the page", "Open de pagina")                                        \
  X(Address, "Address", "Adres")                                                           \
  X(Code, "Code", "Code")                                                                  \
  X(StepOf, "Step %d of 2", "Stap %d van 2")                                               \
  X(Received, "Received %s", "Ontvangen: %s")                                              \
  X(Deleted, "Deleted %s", "Verwijderd: %s")                                               \
  X(Moved, "Moved %s", "Verplaatst: %s")                                                   \
  X(UpdatingFirmware, "Updating firmware...", "Firmware bijwerken...")                     \
  X(FirmwareUpdated, "Firmware updated, restarting", "Firmware bijgewerkt, herstart")      \
  X(SettingsChanged, "Settings changed", "Instellingen gewijzigd")                         \
  /* bookmarks */                                                                          \
  X(BookmarkHere, "Bookmark this page", "Bladwijzer op deze pagina")                       \
  X(RemoveBookmarkHere, "Remove bookmark here", "Bladwijzer hier weghalen")                \
  /* lock */                                                                               \
  X(Locked, "Locked", "Vergrendeld")                                                       \
  X(UnlockMenu, "hold MENU", "houd MENU vast")                \
  X(UnlockHold, "long hold", "lang vasthouden")               \
  X(UnlockTouch, "long press", "lang drukken")                \
  /* statistics */                                                                         \
  X(Today, "Today", "Vandaag")                                                             \
  X(Streak, "Streak", "Reeks")                                                             \
  X(DaysRead, "Days read", "Dagen gelezen")                                                \
  X(Total, "Total", "Totaal")                                                              \
  X(ThisBook, "This book", "Dit boek")                                                     \
  X(PagesMinutes, "%u pages  \xC2\xB7  %u min", "%u pagina's  \xC2\xB7  %u min")           \
  X(PagesHours, "%u pages  \xC2\xB7  %u h %u min", "%u pagina's  \xC2\xB7  %u u %u min")    \
  X(StreakValue, "%u days  \xC2\xB7  best %u", "%u dagen  \xC2\xB7  beste %u")             \
  X(ClockNotSet, "clock not set: start Wi-Fi once", "klok niet gezet: start de wifi")      \
  /* settings */                                                                           \
  X(SetSleepScreen, "Sleep screen", "Slaapscherm")                                         \
  X(SetStatusBar, "Status bar", "Statusbalk")                                              \
  X(SetHoldMenu, "Hold MENU", "MENU vasthouden")                                           \
  X(SetLongHold, "Long hold", "Lang vasthouden")                                           \
  X(SetLongPress, "Long press", "Lang drukken")                                            \
  X(SetLockOnSleep, "Lock when asleep", "Vergrendel bij slapen")                           \
  X(SetLanguage, "Language", "Taal")                                                       \
  X(SetFullRefresh, "Full refresh", "Volledig verversen")                                  \
  X(SetHoldTime, "Hold time", "Vasthoudtijd")                                              \
  X(SetIdleSleep, "Idle sleep", "Slapen na")                                               \
  X(SetSleepAfterPage, "Sleep after page", "Slapen na elke pagina")                        \
  X(SetWakeRefresh, "Wake refresh", "Verversen na wekken")                                 \
  X(EveryN, "every %u", "elke %u")                                                         \
  X(ChaptersOnly, "chapters only", "alleen hoofdstukken")                                  \
  X(On, "on", "aan")                                                                       \
  X(Off, "off", "uit")                                                                     \
  X(WakeRewrite, "rewrite", "herschrijven")                                                \
  X(WakeRetain, "retain", "bewaren")                                                       \
  X(WakeFull, "full", "volledig")                                                          \
  X(ScreenBook, "book", "boek")                                                            \
  X(ScreenCover, "cover", "omslag")                                                        \
  X(ScreenPictures, "pictures", "afbeeldingen")                                            \
  X(ScreenPage, "keep page", "pagina laten staan")                                         \
  X(BarFull, "full", "volledig")                                                           \
  X(BarMinimal, "minimal", "minimaal")                                                     \
  X(BarHidden, "hidden", "verborgen")                                                      \
  X(HoldSleep, "sleep", "slapen")                                                          \
  X(HoldBookmark, "bookmark", "bladwijzer")                                                \
  X(HoldLock, "lock", "vergrendelen")                                                      \
  X(HoldOpenMenu, "menu", "menu")

enum class S : uint16_t {
#define T5_STRING_ID(id, en, nl) id,
  T5_STRINGS(T5_STRING_ID)
#undef T5_STRING_ID
      Count
};

void setLanguage(Lang lang);
Lang language();
const char *tr(S id);
const char *tr(S id, Lang lang);
const char *languageName(Lang lang);  // in that language: "English", "Nederlands"
