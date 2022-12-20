/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2016 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "kwin_wayland_test.h"

#include "core/outputbackend.h"
#include "cursor.h"
#include "input.h"
#include "internalwindow.h"
#include "keyboard_input.h"
#include "useractions.h"
#include "wayland/keyboard_interface.h"
#include "wayland/seat_interface.h"
#include "wayland_server.h"
#include "workspace.h"
#include "x11window.h"
#include "xkb.h"

#include <KWayland/Client/surface.h>

#include <KGlobalAccel>

#include <QAction>

#include <linux/input.h>
#include <netwm.h>
#include <xcb/xcb_icccm.h>

using namespace KWin;

static const QString s_socketName = QStringLiteral("wayland_test_kwin_globalshortcuts-0");

static const struct {const char shortName[3]; const QByteArray longName;} layouts[] = {
    // It's important the first layout is "US-compatible",
    // otherwise default shortcuts might not work
    // as keycode to QtKey mapping wouldn't match.
    {"us", QByteArrayLiteral("English (US)")},

    // QTBUG-90611
    // KEY_GRAVE ("`") has a "ё" symbol here
    {"ru", QByteArrayLiteral("Russian")},

    // QTBUG-108761
    // KEY_GRAVE is a circumflex accent dead key here
    {"de", QByteArrayLiteral("German")},
    // KEY_GRAVE is a Qt::Key_Semicolon (";") on Czech and Hebrew layouts
//    {"cz", QByteArrayLiteral("Czech")},
    // KEY_Q -> Qt::Key_Slash ("/"), KEY_W -> Qt::Key_Apostrophe ("'")
    {"il", QByteArrayLiteral("Hebrew")},

    // FIXME: due to libxkbcommon artificial restriction we can't add more than 4 layouts:
    // https://github.com/xkbcommon/libxkbcommon/issues/311
};

class GlobalShortcutsTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void testNonLatinLayout_data();
    void testNonLatinLayout();
    void testConsumedShift();
    void testRepeatedTrigger();
    void testUserActionsMenu();
    void testMetaShiftW();
    void testComponseKey();
    void testX11WindowShortcut();
    void testWaylandWindowShortcut();
    void testSetupWindowShortcut();
};

void GlobalShortcutsTest::initTestCase()
{
    // to overcome kf.i18n flood
    KLocalizedString::setApplicationDomain("fooapp");

    QByteArray layoutsList;
    for (const auto &l : layouts) {
        (layoutsList += l.shortName) += ',';
    }
    qputenv("KWIN_XKB_DEFAULT_KEYMAP", "1");
    qputenv("XKB_DEFAULT_RULES", "evdev");
    qputenv("XKB_DEFAULT_LAYOUT", layoutsList);

    qRegisterMetaType<KWin::Window *>();
    qRegisterMetaType<KWin::InternalWindow *>();
    QSignalSpy applicationStartedSpy(kwinApp(), &Application::started);
    QVERIFY(waylandServer()->init(s_socketName));
    QMetaObject::invokeMethod(kwinApp()->outputBackend(), "setVirtualOutputs", Qt::DirectConnection, Q_ARG(QVector<QRect>, QVector<QRect>() << QRect(0, 0, 1280, 1024) << QRect(1280, 0, 1280, 1024)));

    kwinApp()->setConfig(KSharedConfig::openConfig(QString(), KConfig::SimpleConfig));
    kwinApp()->start();
    QVERIFY(applicationStartedSpy.wait());
}

void GlobalShortcutsTest::init()
{
    QVERIFY(Test::setupWaylandConnection());
    workspace()->setActiveOutput(QPoint(640, 512));
    KWin::Cursors::self()->mouse()->setPos(QPoint(640, 512));

    auto xkb = input()->keyboard()->xkb();
    xkb->switchToLayout(0);
}

void GlobalShortcutsTest::cleanup()
{
    Test::destroyWaylandConnection();
}

Q_DECLARE_METATYPE(Qt::Modifier)

void GlobalShortcutsTest::testNonLatinLayout_data()
{
    QTest::addColumn<int>("modifierKey");
    QTest::addColumn<Qt::Modifier>("qtModifier");
    QTest::addColumn<int>("key");
    QTest::addColumn<Qt::Key>("qtKey");

    for (const auto &[modifier, qtModifier] :
         QVector<QPair<int, Qt::Modifier>>{
             {KEY_LEFTCTRL, Qt::CTRL},
//             {KEY_RIGHTCTRL, Qt::CTRL},	// this works also
             {KEY_LEFTALT, Qt::ALT},
             {KEY_LEFTSHIFT, Qt::SHIFT},
             {KEY_LEFTMETA, Qt::META},
         }) {
        for (const auto &[key, qtKey] :
             QVector<QPair<int, Qt::Key>> {
                 // Tab is example of a key usually the same on different layouts, check it first
                 {KEY_TAB, qtModifier != Qt::SHIFT ? Qt::Key_Tab : Qt::Key_Backtab},

                 // Then check a key with a Latin letter.
                 // The symbol will probably differ on non-Latin layout.
                 // On Russian layout, KEY_W has a Cyrillic letter "ц", see BUG 375518.
                 // On Hebrew layout, it become other Latin symbol "'", see QTBUG-108761
                 {KEY_W, Qt::Key_W},

                 // KEY_Y is "z" on German layout (QWERTZ), so QTBUG-108761
                 {KEY_Y, Qt::Key_Y},

                 // More common case with any Latin1 symbol keys, including punctuation, should work also.
                 // KEY_GRAVE ("`") key has a "ё" letter on Russian layout, see QTBUG-90611.
                 // It's also a circumflex dead key ("^") on German layout,
                 // and has ";" symbol on Czech and Hebrew layouts: QTBUG-108761
                 {KEY_GRAVE, qtModifier != Qt::SHIFT ? Qt::Key_QuoteLeft : Qt::Key_AsciiTilde},

                 {KEY_1, qtModifier != Qt::SHIFT ? Qt::Key_1 : Qt::Key_Exclam},
                 // note shifted KEY_2 has different Latin symbol on Russian layout - '"' vs "@", so QTBUG-108761
                 {KEY_2, qtModifier != Qt::SHIFT ? Qt::Key_2 : Qt::Key_At},
             }) {
            // remove Shift modifier is it's consumed (see BUG 370341 for why to check isletter() here)
            auto possiblyConsumedModifier = qtModifier == Qt::SHIFT && !QChar::isLetter(qtKey) ? Qt::Modifier() : qtModifier;
            QTest::newRow(QKeySequence(possiblyConsumedModifier + qtKey).toString().toLatin1())
                << modifier << possiblyConsumedModifier
                << key << qtKey;
        }
    }
}

void GlobalShortcutsTest::testNonLatinLayout()
{
    // Shortcuts on non-Latin layouts should still work, see BUG 375518.
    // Also tests some problematic Latin-derived layouts

    QFETCH(int, modifierKey);
    QFETCH(Qt::Modifier, qtModifier);
    QFETCH(int, key);
    QFETCH(Qt::Key, qtKey);

    const QKeySequence seq(qtModifier + qtKey);

    std::unique_ptr<QAction> action(new QAction(nullptr));
    action->setProperty("componentName", QStringLiteral("kwin"));
    action->setObjectName("globalshortcuts-test-non-latin-layout");

    QSignalSpy triggeredSpy(action.get(), &QAction::triggered);

    KGlobalAccel::self()->stealShortcutSystemwide(seq);
    KGlobalAccel::self()->setShortcut(action.get(), {seq}, KGlobalAccel::NoAutoloading);

    // FIXME: workaround for some unexpected fails on English layout after Modifier+<dead key> combination on German.
    // If no shortcut has triggered, the dead key seem continues it's influence even on new layout, modifying keysym produced by the next key press.
    // Pressing the key cancels the behavior for consequent presses.
    // Doesn't needed with QTBUG-108761 patch applied
    quint32 timestamp = 0;
    Test::keyboardKeyPressed(key, timestamp++);
    Test::keyboardKeyReleased(key, timestamp++);

    auto xkb = input()->keyboard()->xkb();
    for (uint layoutIndex = 0; const auto &layout : layouts) {
        xkb->switchToLayout(layoutIndex++);
        QCOMPARE(xkb->layoutName(), layout.longName);

        Test::keyboardKeyPressed(modifierKey, timestamp++);
        Test::keyboardKeyPressed(key, timestamp++);

        QString s(QChar('\n'));
        char keysymName[64];
        xkb_keysym_get_name(xkb->currentKeysym(), keysymName, 64);
        QDebug(&s) << input()->keyboardModifiers() << xkb->modifiersRelevantForGlobalShortcuts(key) << Qt::KeyboardModifiers(qtModifier) << keysymName;

        // passing keycode so the function returns precise result
        QVERIFY2(xkb->modifiersRelevantForGlobalShortcuts(key) == qtModifier, s.toLatin1());

        Test::keyboardKeyReleased(key, timestamp++);
        Test::keyboardKeyReleased(modifierKey, timestamp++);

        QTRY_VERIFY2_WITH_TIMEOUT(triggeredSpy.count(), "Probably you have unpatched Qt, see QTBUG-90611 and QTBUG-108761. Current layout: " + layout.longName + s.toLatin1(), 100);
        triggeredSpy.clear();
    }
}

void GlobalShortcutsTest::testConsumedShift()
{
    // this test verifies that a shortcut with a consumed shift modifier triggers
    // create the action
    std::unique_ptr<QAction> action(new QAction(nullptr));
    action->setProperty("componentName", QStringLiteral("kwin"));
    action->setObjectName(QStringLiteral("globalshortcuts-test-consumed-shift"));
    QSignalSpy triggeredSpy(action.get(), &QAction::triggered);
    KGlobalAccel::self()->setShortcut(action.get(), QList<QKeySequence>{Qt::Key_Percent}, KGlobalAccel::NoAutoloading);

    // press shift+5
    quint32 timestamp = 0;
    Test::keyboardKeyPressed(KEY_LEFTSHIFT, timestamp++);
    QCOMPARE(input()->keyboardModifiers(), Qt::ShiftModifier);
    Test::keyboardKeyPressed(KEY_5, timestamp++);
    QTRY_COMPARE(triggeredSpy.count(), 1);
    Test::keyboardKeyReleased(KEY_5, timestamp++);

    // release shift
    Test::keyboardKeyReleased(KEY_LEFTSHIFT, timestamp++);
}

void GlobalShortcutsTest::testRepeatedTrigger()
{
    // this test verifies that holding a key, triggers repeated global shortcut
    // in addition pressing another key should stop triggering the shortcut

    std::unique_ptr<QAction> action(new QAction(nullptr));
    action->setProperty("componentName", QStringLiteral("kwin"));
    action->setObjectName(QStringLiteral("globalshortcuts-test-consumed-shift"));
    QSignalSpy triggeredSpy(action.get(), &QAction::triggered);
    KGlobalAccel::self()->setShortcut(action.get(), QList<QKeySequence>{Qt::Key_Percent}, KGlobalAccel::NoAutoloading);

    // we need to configure the key repeat first. It is only enabled on libinput
    waylandServer()->seat()->keyboard()->setRepeatInfo(25, 300);

    // press shift+5
    quint32 timestamp = 0;
    Test::keyboardKeyPressed(KEY_WAKEUP, timestamp++);
    Test::keyboardKeyPressed(KEY_LEFTSHIFT, timestamp++);
    QCOMPARE(input()->keyboardModifiers(), Qt::ShiftModifier);
    Test::keyboardKeyPressed(KEY_5, timestamp++);
    QTRY_COMPARE(triggeredSpy.count(), 1);
    // and should repeat
    QVERIFY(triggeredSpy.wait());
    QVERIFY(triggeredSpy.wait());
    // now release the key
    Test::keyboardKeyReleased(KEY_5, timestamp++);
    QVERIFY(!triggeredSpy.wait(50));

    Test::keyboardKeyReleased(KEY_WAKEUP, timestamp++);
    QVERIFY(!triggeredSpy.wait(50));

    // release shift
    Test::keyboardKeyReleased(KEY_LEFTSHIFT, timestamp++);
}

void GlobalShortcutsTest::testUserActionsMenu()
{
    // this test tries to trigger the user actions menu with Alt+F3
    // the problem here is that pressing F3 consumes modifiers as it's part of the
    // Ctrl+alt+F3 keysym for vt switching. xkbcommon considers all modifiers as consumed
    // which a transformation to any keysym would cause
    // for more information see:
    // https://bugs.freedesktop.org/show_bug.cgi?id=92818
    // https://github.com/xkbcommon/libxkbcommon/issues/17

    // first create a window
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);
    QVERIFY(window);
    QVERIFY(window->isActive());

    quint32 timestamp = 0;
    QVERIFY(!workspace()->userActionsMenu()->isShown());
    Test::keyboardKeyPressed(KEY_LEFTALT, timestamp++);
    Test::keyboardKeyPressed(KEY_F3, timestamp++);
    Test::keyboardKeyReleased(KEY_F3, timestamp++);
    QTRY_VERIFY(workspace()->userActionsMenu()->isShown());
    Test::keyboardKeyReleased(KEY_LEFTALT, timestamp++);
}

void GlobalShortcutsTest::testMetaShiftW()
{
    // BUG 370341
    std::unique_ptr<QAction> action(new QAction(nullptr));
    action->setProperty("componentName", QStringLiteral("kwin"));
    action->setObjectName(QStringLiteral("globalshortcuts-test-meta-shift-w"));
    QSignalSpy triggeredSpy(action.get(), &QAction::triggered);
    KGlobalAccel::self()->setShortcut(action.get(), QList<QKeySequence>{Qt::META | Qt::SHIFT | Qt::Key_W}, KGlobalAccel::NoAutoloading);

    // press meta+shift+w
    quint32 timestamp = 0;
    Test::keyboardKeyPressed(KEY_LEFTMETA, timestamp++);
    QCOMPARE(input()->keyboardModifiers(), Qt::MetaModifier);
    Test::keyboardKeyPressed(KEY_LEFTSHIFT, timestamp++);
    QCOMPARE(input()->keyboardModifiers(), Qt::ShiftModifier | Qt::MetaModifier);
    Test::keyboardKeyPressed(KEY_W, timestamp++);
    QTRY_COMPARE(triggeredSpy.count(), 1);
    Test::keyboardKeyReleased(KEY_W, timestamp++);

    // release meta+shift
    Test::keyboardKeyReleased(KEY_LEFTSHIFT, timestamp++);
    Test::keyboardKeyReleased(KEY_LEFTMETA, timestamp++);
}

void GlobalShortcutsTest::testComponseKey()
{
    // BUG 390110
    std::unique_ptr<QAction> action(new QAction(nullptr));
    action->setProperty("componentName", QStringLiteral("kwin"));
    action->setObjectName(QStringLiteral("globalshortcuts-accent"));
    QSignalSpy triggeredSpy(action.get(), &QAction::triggered);
    KGlobalAccel::self()->setShortcut(action.get(), QList<QKeySequence>{Qt::NoModifier}, KGlobalAccel::NoAutoloading);

    // press & release `
    quint32 timestamp = 0;
    Test::keyboardKeyPressed(KEY_RESERVED, timestamp++);
    Test::keyboardKeyReleased(KEY_RESERVED, timestamp++);

    QTRY_COMPARE(triggeredSpy.count(), 0);
}

struct XcbConnectionDeleter
{
    void operator()(xcb_connection_t *pointer)
    {
        xcb_disconnect(pointer);
    }
};

void GlobalShortcutsTest::testX11WindowShortcut()
{
#ifdef NO_XWAYLAND
    QSKIP("x11 test, unnecessary without xwayland");
#endif
    // create an X11 window
    std::unique_ptr<xcb_connection_t, XcbConnectionDeleter> c(xcb_connect(nullptr, nullptr));
    QVERIFY(!xcb_connection_has_error(c.get()));
    xcb_window_t windowId = xcb_generate_id(c.get());
    const QRect windowGeometry = QRect(0, 0, 10, 20);
    const uint32_t values[] = {
        XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW};
    xcb_create_window(c.get(), XCB_COPY_FROM_PARENT, windowId, rootWindow(),
                      windowGeometry.x(),
                      windowGeometry.y(),
                      windowGeometry.width(),
                      windowGeometry.height(),
                      0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, values);
    xcb_size_hints_t hints;
    memset(&hints, 0, sizeof(hints));
    xcb_icccm_size_hints_set_position(&hints, 1, windowGeometry.x(), windowGeometry.y());
    xcb_icccm_size_hints_set_size(&hints, 1, windowGeometry.width(), windowGeometry.height());
    xcb_icccm_set_wm_normal_hints(c.get(), windowId, &hints);
    NETWinInfo info(c.get(), windowId, rootWindow(), NET::WMAllProperties, NET::WM2AllProperties);
    info.setWindowType(NET::Normal);
    xcb_map_window(c.get(), windowId);
    xcb_flush(c.get());

    QSignalSpy windowCreatedSpy(workspace(), &Workspace::windowAdded);
    QVERIFY(windowCreatedSpy.wait());
    X11Window *window = windowCreatedSpy.last().first().value<X11Window *>();
    QVERIFY(window);

    QCOMPARE(workspace()->activeWindow(), window);
    QVERIFY(window->isActive());
    QCOMPARE(window->shortcut(), QKeySequence());
    const QKeySequence seq(Qt::META | Qt::SHIFT | Qt::Key_Y);
    QVERIFY(workspace()->shortcutAvailable(seq));
    window->setShortcut(seq.toString());
    QCOMPARE(window->shortcut(), seq);
    QVERIFY(!workspace()->shortcutAvailable(seq));
    QCOMPARE(window->caption(), QStringLiteral(" {Meta+Shift+Y}"));

    // it's delayed
    QCoreApplication::processEvents();

    workspace()->activateWindow(nullptr);
    QVERIFY(!workspace()->activeWindow());
    QVERIFY(!window->isActive());

    // now let's trigger the shortcut
    quint32 timestamp = 0;
    Test::keyboardKeyPressed(KEY_LEFTMETA, timestamp++);
    Test::keyboardKeyPressed(KEY_LEFTSHIFT, timestamp++);
    Test::keyboardKeyPressed(KEY_Y, timestamp++);
    QTRY_COMPARE(workspace()->activeWindow(), window);
    Test::keyboardKeyReleased(KEY_Y, timestamp++);
    Test::keyboardKeyReleased(KEY_LEFTSHIFT, timestamp++);
    Test::keyboardKeyReleased(KEY_LEFTMETA, timestamp++);

    // destroy window again
    QSignalSpy windowClosedSpy(window, &X11Window::windowClosed);
    xcb_unmap_window(c.get(), windowId);
    xcb_destroy_window(c.get(), windowId);
    xcb_flush(c.get());
    QVERIFY(windowClosedSpy.wait());
}

void GlobalShortcutsTest::testWaylandWindowShortcut()
{
    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);

    QCOMPARE(workspace()->activeWindow(), window);
    QVERIFY(window->isActive());
    QCOMPARE(window->shortcut(), QKeySequence());
    const QKeySequence seq(Qt::META | Qt::SHIFT | Qt::Key_Y);
    QVERIFY(workspace()->shortcutAvailable(seq));
    window->setShortcut(seq.toString());
    QCOMPARE(window->shortcut(), seq);
    QVERIFY(!workspace()->shortcutAvailable(seq));
    QCOMPARE(window->caption(), QStringLiteral(" {Meta+Shift+Y}"));

    workspace()->activateWindow(nullptr);
    QVERIFY(!workspace()->activeWindow());
    QVERIFY(!window->isActive());

    // now let's trigger the shortcut
    quint32 timestamp = 0;
    Test::keyboardKeyPressed(KEY_LEFTMETA, timestamp++);
    Test::keyboardKeyPressed(KEY_LEFTSHIFT, timestamp++);
    Test::keyboardKeyPressed(KEY_Y, timestamp++);
    QTRY_COMPARE(workspace()->activeWindow(), window);
    Test::keyboardKeyReleased(KEY_Y, timestamp++);
    Test::keyboardKeyReleased(KEY_LEFTSHIFT, timestamp++);
    Test::keyboardKeyReleased(KEY_LEFTMETA, timestamp++);

    shellSurface.reset();
    surface.reset();
    QVERIFY(Test::waitForWindowDestroyed(window));
    QTRY_VERIFY_WITH_TIMEOUT(workspace()->shortcutAvailable(seq), 500); // we need the try since KGlobalAccelPrivate::unregister is async
}

void GlobalShortcutsTest::testSetupWindowShortcut()
{
    // QTBUG-62102

    std::unique_ptr<KWayland::Client::Surface> surface(Test::createSurface());
    std::unique_ptr<Test::XdgToplevel> shellSurface(Test::createXdgToplevelSurface(surface.get()));
    auto window = Test::renderAndWaitForShown(surface.get(), QSize(100, 50), Qt::blue);

    QCOMPARE(workspace()->activeWindow(), window);
    QVERIFY(window->isActive());
    QCOMPARE(window->shortcut(), QKeySequence());

    QSignalSpy shortcutDialogAddedSpy(workspace(), &Workspace::internalWindowAdded);
    workspace()->slotSetupWindowShortcut();
    QTRY_COMPARE(shortcutDialogAddedSpy.count(), 1);
    auto dialog = shortcutDialogAddedSpy.first().first().value<InternalWindow *>();
    QVERIFY(dialog);
    QVERIFY(dialog->isInternal());
    auto sequenceEdit = workspace()->shortcutDialog()->findChild<QKeySequenceEdit *>();
    QVERIFY(sequenceEdit);

#if QT_VERSION_MAJOR < 6
    // the QKeySequenceEdit field does not get focus, we need to pass it focus manually
    QEXPECT_FAIL("", "Edit does not have focus", Continue);
    QVERIFY(sequenceEdit->hasFocus());
    sequenceEdit->setFocus();
#endif
    QTRY_VERIFY(sequenceEdit->hasFocus());

    quint32 timestamp = 0;
    Test::keyboardKeyPressed(KEY_LEFTMETA, timestamp++);
    Test::keyboardKeyPressed(KEY_LEFTSHIFT, timestamp++);
    Test::keyboardKeyPressed(KEY_Y, timestamp++);
    Test::keyboardKeyReleased(KEY_Y, timestamp++);
    Test::keyboardKeyReleased(KEY_LEFTSHIFT, timestamp++);
    Test::keyboardKeyReleased(KEY_LEFTMETA, timestamp++);

    // the sequence gets accepted after one second, so wait a bit longer
    QTest::qWait(2000);
    // now send in enter
    Test::keyboardKeyPressed(KEY_ENTER, timestamp++);
    Test::keyboardKeyReleased(KEY_ENTER, timestamp++);
    QTRY_COMPARE(window->shortcut(), QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_Y));
}

WAYLANDTEST_MAIN(GlobalShortcutsTest)
#include "globalshortcuts_test.moc"
