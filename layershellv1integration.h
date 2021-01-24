/*
    SPDX-FileCopyrightText: 2020 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "waylandshellintegration.h"

#include <QVector>

namespace KWaylandServer
{
class LayerSurfaceV1Interface;
}

namespace KWin
{

class LayerShellV1Client;

class LayerShellV1Integration : public WaylandShellIntegration
{
    Q_OBJECT

public:
    explicit LayerShellV1Integration(QObject *parent = nullptr);
    ~LayerShellV1Integration() override;

    void rearrange();
    void scheduleRearrange();

    void scheduleActivate(LayerShellV1Client *client);
    void scheduleDeactivate(LayerShellV1Client *client);

    void createClient(KWaylandServer::LayerSurfaceV1Interface *shellSurface);
    void recreateClient(KWaylandServer::LayerSurfaceV1Interface *shellSurface);
    void destroyClient(KWaylandServer::LayerSurfaceV1Interface *shellSurface);

private:
    void handleDeactivate();
    bool handleActivateExclusive();
    void handleActivateOnDemand();

    QTimer *m_rearrangeTimer;
    QVector<QPointer<LayerShellV1Client>> m_activateQueue;
    QVector<QPointer<LayerShellV1Client>> m_deactivateQueue;
};

} // namespace KWin
