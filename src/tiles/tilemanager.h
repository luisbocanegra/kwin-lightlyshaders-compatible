/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2022 Marco Martin <mart@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef KWIN_CUSTOM_TILING_H
#define KWIN_CUSTOM_TILING_H

#include "customtile.h"
#include "quicktile.h"
#include "tile.h"
#include "utils/common.h"
#include "virtualdesktops.h"
#include <kwin_export.h>

#include <QAbstractItemModel>
#include <QObject>
#include <QRectF>

#include <QJsonValue>

class QTimer;

namespace KWin
{

class Output;
class Tile;
class TileManager;

struct ManagerIndex
{
    VirtualDesktop *desktop;
    QString activity;
};

bool operator==(const ManagerIndex &m1, const ManagerIndex &other);

uint qHash(const ManagerIndex &key, uint seed = 0);

/**
 * Custom tiling zones management per output.
 */
class KWIN_EXPORT TileManager : public QAbstractItemModel
{
    Q_OBJECT
    Q_PROPERTY(KWin::Tile *rootTile READ rootTile CONSTANT)

public:
    enum Roles {
        TileRole = Qt::UserRole + 1
    };

    ~TileManager() override;

    static TileManager *instance(Output *output, VirtualDesktop *desktop, const QString &activity); // TODO, VD and Activity too?

    Output *output() const;

    KWin::Tile *bestTileForPosition(const QPointF &pos);
    Q_INVOKABLE KWin::Tile *bestTileForPosition(qreal x, qreal y); // For scripting
    CustomTile *rootTile() const;
    KWin::Tile *quickTile(QuickTileMode mode) const;

    // QAbstractItemModel overrides
    QHash<int, QByteArray> roleNames() const override;
    QVariant data(const QModelIndex &index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QModelIndex index(int row, int column,
                      const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &index) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;

Q_SIGNALS:
    void tileRemoved(KWin::Tile *tile);

private:
    explicit TileManager(VirtualDesktop *desktop, const QString &activity, Output *parent = nullptr);

    CustomTile *addTile(const QRectF &relativeGeometry, CustomTile::LayoutDirection layoutDirection, int position, CustomTile *parentTile);
    void removeTile(CustomTile *tile);

    void readSettings();
    void saveSettings();
    QJsonObject tileToJSon(CustomTile *parentTile);
    CustomTile *parseTilingJSon(const QJsonValue &val, const QRectF &availableArea, CustomTile *parentTile);

    Q_DISABLE_COPY(TileManager)

    Output *m_output = nullptr;
    VirtualDesktop *m_desktop;
    const QString m_activity;

    QTimer *m_saveTimer = nullptr;
    std::unique_ptr<CustomTile> m_rootTile = nullptr;
    std::unique_ptr<QuickRootTile> m_quickRootTile = nullptr;
    static QHash<Output *, QHash<ManagerIndex, TileManager *>> s_managers;
    friend class CustomTile;
};

KWIN_EXPORT QDebug operator<<(QDebug debug, const TileManager *tileManager);

} // namespace KWin

#endif
