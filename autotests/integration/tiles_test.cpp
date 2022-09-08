/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2022 Marco Martin <mart@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "kwin_wayland_test.h"

#include "core/output.h"
#include "core/platform.h"
#include "cursor.h"
#include "tiles/tilemanager.h"
#include "wayland/seat_interface.h"
#include "wayland/surface_interface.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"
#include <kwineffects.h>

#include <QAbstractItemModelTester>

namespace KWin
{

static const QString s_socketName = QStringLiteral("wayland_test_kwin_transient_placement-0");

class TilesTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();
    void testTileGeometries();
    void testWindowInteraction();
    void testAssignedTileDeletion();
    void resizeTileFomWindow();

private:
    Output *m_output;
    TileManager *m_tileManager;
    CustomTile *m_rootTile;
    KWayland::Client::ConnectionThread *m_connection = nullptr;
    KWayland::Client::Compositor *m_compositor = nullptr;
};

void TilesTest::initTestCase()
{
    qRegisterMetaType<KWin::Window *>();
    QSignalSpy applicationStartedSpy(kwinApp(), &Application::started);
    QVERIFY(applicationStartedSpy.isValid());
    kwinApp()->platform()->setInitialWindowSize(QSize(1280, 1024));
    QVERIFY(waylandServer()->init(s_socketName));
    QMetaObject::invokeMethod(kwinApp()->platform(), "setVirtualOutputs", Qt::DirectConnection, Q_ARG(int, 2));

    kwinApp()->start();
    QVERIFY(applicationStartedSpy.wait());
    const auto outputs = workspace()->outputs();
    QCOMPARE(outputs.count(), 2);
    QCOMPARE(outputs[0]->geometry(), QRect(0, 0, 1280, 1024));
    QCOMPARE(outputs[1]->geometry(), QRect(1280, 0, 1280, 1024));
    setenv("QT_QPA_PLATFORM", "wayland", true);
}

void TilesTest::init()
{
    QVERIFY(Test::setupWaylandConnection(Test::AdditionalWaylandInterface::Decoration | Test::AdditionalWaylandInterface::PlasmaShell | Test::AdditionalWaylandInterface::Seat));
    QVERIFY(Test::waitForWaylandPointer());

    m_connection = Test::waylandConnection();
    m_compositor = Test::waylandCompositor();

    workspace()->setActiveOutput(QPoint(640, 512));
    Cursors::self()->mouse()->setPos(QPoint(640, 512));
    m_output = workspace()->activeOutput();
    m_tileManager = m_output->tileManager();
    m_rootTile = m_tileManager->rootTile();
    QAbstractItemModelTester(m_tileManager, QAbstractItemModelTester::FailureReportingMode::QtTest);
}

void TilesTest::cleanup()
{
    Test::destroyWaylandConnection();
}

void TilesTest::testTileGeometries()
{
    QCOMPARE(m_rootTile->childCount(), 0);
    m_rootTile->split(CustomTile::LayoutDirection::Horizontal);
    QCOMPARE(m_rootTile->childCount(), 2);

    auto leftTile = qobject_cast<CustomTile *>(m_rootTile->childTiles().first());
    auto rightTile = qobject_cast<CustomTile *>(m_rootTile->childTiles().last());
    QVERIFY(leftTile);
    QVERIFY(rightTile);

    QCOMPARE(leftTile->relativeGeometry(), QRectF(0, 0, 0.5, 1));
    QCOMPARE(rightTile->relativeGeometry(), QRectF(0.5, 0, 0.5, 1));

    // Splitting with the same layout direction creates a sibling, not 2 children
    rightTile->split(CustomTile::LayoutDirection::Horizontal);
    auto newRightTile = qobject_cast<CustomTile *>(m_rootTile->childTiles().last());

    QCOMPARE(m_rootTile->childCount(), 3);
    QCOMPARE(m_rootTile->relativeGeometry(), QRectF(0, 0, 1, 1));
    QCOMPARE(leftTile->relativeGeometry(), QRectF(0, 0, 0.5, 1));
    QCOMPARE(rightTile->relativeGeometry(), QRectF(0.5, 0, 0.25, 1));
    QCOMPARE(newRightTile->relativeGeometry(), QRectF(0.75, 0, 0.25, 1));

    QCOMPARE(m_rootTile->windowGeometry(), QRectF(4, 4, 1272, 1016));
    QCOMPARE(leftTile->windowGeometry(), QRectF(4, 4, 632, 1016));
    QCOMPARE(rightTile->windowGeometry(), QRectF(644, 4, 312, 1016));
    QCOMPARE(newRightTile->windowGeometry(), QRectF(964, 4, 312, 1016));

    // Splitting with a different layout direction creates 2 children in the tile
    QVERIFY(!rightTile->isLayout());
    QCOMPARE(rightTile->childCount(), 0);
    rightTile->split(CustomTile::LayoutDirection::Vertical);
    QVERIFY(rightTile->isLayout());
    QCOMPARE(rightTile->childCount(), 2);
    auto verticalTopTile = qobject_cast<CustomTile *>(rightTile->childTiles().first());
    auto verticalBottomTile = qobject_cast<CustomTile *>(rightTile->childTiles().last());

    // geometry of rightTile should be the same
    QCOMPARE(m_rootTile->childCount(), 3);
    QCOMPARE(rightTile->relativeGeometry(), QRectF(0.5, 0, 0.25, 1));
    QCOMPARE(rightTile->windowGeometry(), QRectF(644, 4, 312, 1016));

    QCOMPARE(verticalTopTile->relativeGeometry(), QRectF(0.5, 0, 0.25, 0.5));
    QCOMPARE(verticalBottomTile->relativeGeometry(), QRectF(0.5, 0.5, 0.25, 0.5));
    QCOMPARE(verticalTopTile->windowGeometry(), QRectF(644, 4, 312, 504));
    QCOMPARE(verticalBottomTile->windowGeometry(), QRectF(644, 516, 312, 504));
}

void TilesTest::testWindowInteraction()
{
    std::unique_ptr<KWayland::Client::Surface> rootSurface(Test::createSurface());

    std::unique_ptr<Test::XdgToplevel> root(Test::createXdgToplevelSurface(rootSurface.get()));

    QSignalSpy surfaceConfigureRequestedSpy(root->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigureRequestedSpy(root.get(), &Test::XdgToplevel::configureRequested);

    auto rootWindow = Test::renderAndWaitForShown(rootSurface.get(), QSize(100, 100), Qt::cyan);
    QVERIFY(rootWindow);
    QSignalSpy frameGeometryChangedSpy(rootWindow, &Window::frameGeometryChanged);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);
    QCOMPARE(toplevelConfigureRequestedSpy.count(), 1);
    root->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());

    auto leftTile = qobject_cast<CustomTile *>(m_rootTile->childTiles().first());
    QVERIFY(leftTile);

    rootWindow->setTile(leftTile);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 2);
    QCOMPARE(toplevelConfigureRequestedSpy.count(), 2);

    root->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());

    Test::render(rootSurface.get(), toplevelConfigureRequestedSpy.last().first().value<QSize>(), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(rootWindow->frameGeometry(), leftTile->windowGeometry().toRect());

    QCOMPARE(toplevelConfigureRequestedSpy.last().first().value<QSize>(), leftTile->windowGeometry().toRect().size());

    // Resize owning tile
    leftTile->setRelativeGeometry({0, 0, 0.4, 1});

    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 3);
    QCOMPARE(toplevelConfigureRequestedSpy.count(), 3);

    root->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());

    QCOMPARE(toplevelConfigureRequestedSpy.last().first().value<QSize>(), leftTile->windowGeometry().toRect().size());

    Test::render(rootSurface.get(), toplevelConfigureRequestedSpy.last().first().value<QSize>(), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(rootWindow->frameGeometry(), leftTile->windowGeometry().toRect());

    auto middleTile = qobject_cast<CustomTile *>(m_rootTile->childTiles()[1]);
    QVERIFY(middleTile);
    auto rightTile = qobject_cast<CustomTile *>(m_rootTile->childTiles()[2]);
    QVERIFY(rightTile);
    auto verticalTopTile = qobject_cast<CustomTile *>(middleTile->childTiles().first());
    QVERIFY(verticalTopTile);
    auto verticalBottomTile = qobject_cast<CustomTile *>(middleTile->childTiles().last());
    QVERIFY(verticalBottomTile);

    QCOMPARE(leftTile->relativeGeometry(), QRectF(0, 0, 0.4, 1));
    QCOMPARE(middleTile->relativeGeometry(), QRectF(0.4, 0, 0.35, 1));
    QCOMPARE(rightTile->relativeGeometry(), QRectF(0.75, 0, 0.25, 1));
    QCOMPARE(verticalTopTile->relativeGeometry(), QRectF(0.4, 0, 0.35, 0.5));
    QCOMPARE(verticalBottomTile->relativeGeometry(), QRectF(0.4, 0.5, 0.35, 0.5));
}

void TilesTest::testAssignedTileDeletion()
{
    std::unique_ptr<KWayland::Client::Surface> rootSurface(Test::createSurface());

    std::unique_ptr<Test::XdgToplevel> root(Test::createXdgToplevelSurface(rootSurface.get()));

    QSignalSpy surfaceConfigureRequestedSpy(root->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigureRequestedSpy(root.get(), &Test::XdgToplevel::configureRequested);

    auto rootWindow = Test::renderAndWaitForShown(rootSurface.get(), QSize(100, 100), Qt::cyan);
    QVERIFY(rootWindow);
    QSignalSpy frameGeometryChangedSpy(rootWindow, &Window::frameGeometryChanged);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);
    QCOMPARE(toplevelConfigureRequestedSpy.count(), 1);
    root->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());

    auto middleTile = qobject_cast<CustomTile *>(m_rootTile->childTiles()[1]);
    QVERIFY(middleTile);
    auto middleBottomTile = qobject_cast<CustomTile *>(m_rootTile->childTiles()[1]->childTiles()[1]);
    QVERIFY(middleBottomTile);

    rootWindow->setTile(middleBottomTile);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 2);
    QCOMPARE(toplevelConfigureRequestedSpy.count(), 2);

    root->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());

    Test::render(rootSurface.get(), toplevelConfigureRequestedSpy.last().first().value<QSize>(), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(rootWindow->frameGeometry(), middleBottomTile->windowGeometry().toRect());

    QCOMPARE(toplevelConfigureRequestedSpy.last().first().value<QSize>(), middleBottomTile->windowGeometry().toRect().size());

    QCOMPARE(middleBottomTile->windowGeometry().toRect(), QRect(516, 516, 440, 504));

    middleBottomTile->remove();

    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 3);
    QCOMPARE(toplevelConfigureRequestedSpy.count(), 3);

    root->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());

    // The window has been reassigned to middleTile after deletion of the children
    QCOMPARE(toplevelConfigureRequestedSpy.last().first().value<QSize>(), middleTile->windowGeometry().toRect().size());

    Test::render(rootSurface.get(), toplevelConfigureRequestedSpy.last().first().value<QSize>(), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(rootWindow->frameGeometry(), middleTile->windowGeometry().toRect());

    // Both children have been deleted as the system avoids tiles with ha single child
    QCOMPARE(middleTile->isLayout(), false);
    QCOMPARE(middleTile->childCount(), 0);
    QCOMPARE(rootWindow->tile(), middleTile);
}

void TilesTest::resizeTileFomWindow()
{
    std::unique_ptr<KWayland::Client::Surface> rootSurface(Test::createSurface());

    std::unique_ptr<Test::XdgToplevel> root(Test::createXdgToplevelSurface(rootSurface.get()));

    QSignalSpy surfaceConfigureRequestedSpy(root->xdgSurface(), &Test::XdgSurface::configureRequested);
    QSignalSpy toplevelConfigureRequestedSpy(root.get(), &Test::XdgToplevel::configureRequested);

    Test::XdgToplevel::States states;
    auto window = Test::renderAndWaitForShown(rootSurface.get(), QSize(100, 100), Qt::cyan);
    QVERIFY(window);
    QSignalSpy frameGeometryChangedSpy(window, &Window::frameGeometryChanged);
    QVERIFY(frameGeometryChangedSpy.isValid());
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 1);
    QCOMPARE(toplevelConfigureRequestedSpy.count(), 1);
    root->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());

    auto leftTile = qobject_cast<CustomTile *>(m_rootTile->childTiles().first());
    QVERIFY(leftTile);
    QCOMPARE(leftTile->windowGeometry(), QRectF(4, 4, 504, 1016));

    auto middleTile = qobject_cast<CustomTile *>(m_rootTile->childTiles()[1]);
    QVERIFY(middleTile);
    QCOMPARE(middleTile->windowGeometry(), QRectF(516, 4, 440, 1016));

    leftTile->split(CustomTile::LayoutDirection::Vertical);
    auto topLeftTile = qobject_cast<CustomTile *>(leftTile->childTiles().first());
    QVERIFY(topLeftTile);
    QCOMPARE(topLeftTile->windowGeometry(), QRectF(4, 4, 504, 504));
    QSignalSpy tileGeometryChangedSpy(topLeftTile, &Tile::windowGeometryChanged);
    auto bottomLeftTile = qobject_cast<CustomTile *>(leftTile->childTiles().last());
    QVERIFY(bottomLeftTile);
    QCOMPARE(bottomLeftTile->windowGeometry(), QRectF(4, 516, 504, 504));

    window->setTile(topLeftTile);
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 2);
    QCOMPARE(toplevelConfigureRequestedSpy.count(), 2);

    root->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());

    QCOMPARE(toplevelConfigureRequestedSpy.last().first().value<QSize>(), topLeftTile->windowGeometry().toRect().size());
    Test::render(rootSurface.get(), toplevelConfigureRequestedSpy.last().first().value<QSize>(), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->frameGeometry(), QRect(4, 4, 504, 504));

    // effects signal handlers
    QSignalSpy windowStartUserMovedResizedSpy(effects, &EffectsHandler::windowStartUserMovedResized);
    QVERIFY(windowStartUserMovedResizedSpy.isValid());
    QSignalSpy windowStepUserMovedResizedSpy(effects, &EffectsHandler::windowStepUserMovedResized);
    QVERIFY(windowStepUserMovedResizedSpy.isValid());
    QSignalSpy windowFinishUserMovedResizedSpy(effects, &EffectsHandler::windowFinishUserMovedResized);
    QVERIFY(windowFinishUserMovedResizedSpy.isValid());

    QCOMPARE(workspace()->activeWindow(), window);
    QSignalSpy startMoveResizedSpy(window, &Window::clientStartUserMovedResized);
    QVERIFY(startMoveResizedSpy.isValid());
    QSignalSpy moveResizedChangedSpy(window, &Window::moveResizedChanged);
    QVERIFY(moveResizedChangedSpy.isValid());
    QSignalSpy clientStepUserMovedResizedSpy(window, &Window::clientStepUserMovedResized);
    QVERIFY(clientStepUserMovedResizedSpy.isValid());
    QSignalSpy clientFinishUserMovedResizedSpy(window, &Window::clientFinishUserMovedResized);
    QVERIFY(clientFinishUserMovedResizedSpy.isValid());

    // begin resize
    QCOMPARE(workspace()->moveResizeWindow(), nullptr);
    QCOMPARE(window->isInteractiveMove(), false);
    QCOMPARE(window->isInteractiveResize(), false);
    workspace()->slotWindowResize();
    QCOMPARE(workspace()->moveResizeWindow(), window);
    QCOMPARE(startMoveResizedSpy.count(), 1);
    QCOMPARE(moveResizedChangedSpy.count(), 1);
    QCOMPARE(window->isInteractiveResize(), true);
    QCOMPARE(window->geometryRestore(), QRect());
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 3);
    QCOMPARE(toplevelConfigureRequestedSpy.count(), 3);
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Activated));
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Resizing));

    // Trigger a change.
    QPoint cursorPos = Cursors::self()->mouse()->pos();
    window->keyPressEvent(Qt::Key_Right); // FIXME: find a way to start an event with supported gravity instead
    window->updateInteractiveMoveResize(Cursors::self()->mouse()->pos());
    QCOMPARE(Cursors::self()->mouse()->pos(), cursorPos + QPoint(8, 0));

    // The client should receive a configure event with the new size.
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 4);
    QCOMPARE(toplevelConfigureRequestedSpy.count(), 4);
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Activated));
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Resizing));
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(512, 504));
    QCOMPARE(clientStepUserMovedResizedSpy.count(), 1);

    // Now render new size.
    root->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(rootSurface.get(), toplevelConfigureRequestedSpy.last().first().value<QSize>(), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->frameGeometry(), QRect(4, 4, 512, 504));
    QCOMPARE(clientStepUserMovedResizedSpy.count(), 1);

    QTRY_COMPARE(tileGeometryChangedSpy.count(), 1);
    QCOMPARE(window->tile(), topLeftTile);
    QCOMPARE(topLeftTile->windowGeometry(), QRect(4, 4, 512, 504));
    QCOMPARE(bottomLeftTile->windowGeometry(), QRect(4, 516, 512, 504));
    QCOMPARE(leftTile->windowGeometry(), QRect(4, 4, 512, 1016));
    QCOMPARE(middleTile->windowGeometry(), QRect(524, 4, 432, 1016));

    // Resize vertically
    workspace()->slotWindowResize();
    QCOMPARE(workspace()->moveResizeWindow(), window);
    QCOMPARE(startMoveResizedSpy.count(), 2);
    QCOMPARE(moveResizedChangedSpy.count(), 3);
    QCOMPARE(window->isInteractiveResize(), true);
    QCOMPARE(window->geometryRestore(), QRect());
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 5);
    QCOMPARE(toplevelConfigureRequestedSpy.count(), 5);
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Activated));
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Resizing));

    // Trigger a change.
    cursorPos = Cursors::self()->mouse()->pos();
    window->keyPressEvent(Qt::Key_Down); // FIXME: find a way to start an event with supported gravity instead
    window->updateInteractiveMoveResize(Cursors::self()->mouse()->pos());
    QCOMPARE(Cursors::self()->mouse()->pos(), cursorPos + QPoint(0, 8));

    // The client should receive a configure event with the new size.
    QVERIFY(surfaceConfigureRequestedSpy.wait());
    QCOMPARE(surfaceConfigureRequestedSpy.count(), 6);
    QCOMPARE(toplevelConfigureRequestedSpy.count(), 6);
    states = toplevelConfigureRequestedSpy.last().at(1).value<Test::XdgToplevel::States>();
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Activated));
    QVERIFY(states.testFlag(Test::XdgToplevel::State::Resizing));
    QCOMPARE(toplevelConfigureRequestedSpy.last().at(0).toSize(), QSize(512, 512));
    QCOMPARE(clientStepUserMovedResizedSpy.count(), 2);

    // Now render new size.
    root->xdgSurface()->ack_configure(surfaceConfigureRequestedSpy.last().at(0).value<quint32>());
    Test::render(rootSurface.get(), toplevelConfigureRequestedSpy.last().first().value<QSize>(), Qt::blue);
    QVERIFY(frameGeometryChangedSpy.wait());
    QCOMPARE(window->frameGeometry(), QRect(4, 4, 512, 512));
    QCOMPARE(clientStepUserMovedResizedSpy.count(), 2);

    QTRY_COMPARE(tileGeometryChangedSpy.count(), 2);
    QCOMPARE(window->tile(), topLeftTile);
    QCOMPARE(topLeftTile->windowGeometry(), QRect(4, 4, 512, 512));
    QCOMPARE(bottomLeftTile->windowGeometry(), QRect(4, 524, 512, 496));
    QCOMPARE(leftTile->windowGeometry(), QRect(4, 4, 512, 1016));
    QCOMPARE(middleTile->windowGeometry(), QRect(524, 4, 432, 1016));
}
}

WAYLANDTEST_MAIN(KWin::TilesTest)
#include "tiles_test.moc"
