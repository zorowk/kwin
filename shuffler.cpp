/*
    SPDX-FileCopyrightText: 2020 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "shuffler.h"
#include "abstract_client.h"
#include "abstract_output.h"
#include "main.h"
#include "platform.h"
#include "screens.h"
#include "workspace.h"

namespace KWin
{

Shuffler::Shuffler(QObject *parent)
    : QObject(parent)
{
    connect(workspace(), &Workspace::clientAdded, this, &Shuffler::handleClientAdded);
    connect(workspace(), &Workspace::clientRemoved, this, &Shuffler::handleClientRemoved);
}

static QRect moveBetweenRects(const QRect &sourceArea, const QRect &targetArea, const QRect &rect)
{
    Q_ASSERT(sourceArea.isValid() && targetArea.isValid());

    const int xOffset = rect.x() - sourceArea.x();
    const int yOffset = rect.y() - sourceArea.y();

    const qreal xScale = qreal(targetArea.width()) / sourceArea.width();
    const qreal yScale = qreal(targetArea.height()) / sourceArea.height();

    return QRectF(targetArea.x() + xOffset * xScale, targetArea.y() + yOffset * yScale,
                  rect.width() * xScale, rect.height() * yScale).toRect();
}

void Shuffler::shuffle()
{
    for (auto it = m_shuffleData.constBegin(); it != m_shuffleData.constEnd(); ++it) {
        AbstractClient *window = it.key();
        const ShuffleData &data = it.value();

        QRect originalArea;
        QRect originalRect;
        QRect targetArea;

        if (AbstractOutput *output = kwinApp()->platform()->findOutput(data.savedOutputName)) {
            targetArea = output->geometry();
            originalArea = data.savedOutputRect;
            originalRect = data.savedGeometry;
        } else if (AbstractOutput *output = kwinApp()->platform()->findOutput(data.lastOutputName)) {
            targetArea = output->geometry();
            originalArea = data.lastOutputRect;
            originalRect = data.lastGeometry;
        } else {
            qCDebug(KWIN_CORE) << "Could not find the original output for window" << window;
            continue;
        }
        if (originalArea == targetArea) {
            continue;
        }

        if (!originalRect.isValid()) {
            qCDebug(KWIN_CORE) << "Not re-arranging" << window << "due to invalid geometry";
            continue;
        }

        window->setFrameGeometry(moveBetweenRects(originalArea, targetArea, originalRect));
    }
}

void Shuffler::handleClientAdded(AbstractClient *client)
{
    if (client->isSpecialWindow() || client->isPopupWindow() || !client->isPlaceable()) {
        return;
    }

    connect(client, &AbstractClient::clientFinishUserMovedResized,
            this, &Shuffler::updateFullShuffleState);
    connect(client, &AbstractClient::frameGeometryChanged,
            this, [client, this]() { updateLastShuffleState(client); });
    connect(client, &AbstractClient::fullScreenChanged,
            this, [client, this]() { updateFullShuffleState(client); });
    connect(client, qOverload<AbstractClient *, MaximizeMode>(&AbstractClient::clientMaximizedStateChanged),
            this, &Shuffler::updateFullShuffleState);
    connect(client, &AbstractClient::quickTileModeChanged,
            this, [client, this]() { updateFullShuffleState(client); });
    connect(client, &AbstractClient::sentToScreen,
            this, [client, this]() { updateFullShuffleState(client); });

    m_shuffleData.insert(client, ShuffleData());

    updateFullShuffleState(client);
}

void Shuffler::handleClientRemoved(AbstractClient *client)
{
    m_shuffleData.remove(client);
}

static void updateSavedShuffleState_helper(ShuffleData *data)
{
    data->savedOutputName = data->lastOutputName;
    data->savedGeometry = data->lastGeometry;
    data->savedOutputRect = data->lastOutputRect;
}

static void updateLastShuffleState_helper(ShuffleData *data, AbstractClient *window)
{
    const int screenId = screens()->number(window->frameGeometry().center());
    if (screenId == -1) {
        qCWarning(KWIN_CORE) << "Could not find any output for window" << window;
        return;
    }

    const AbstractOutput *output = kwinApp()->platform()->findOutput(screenId);

    data->lastOutputName = output->name();
    data->lastGeometry = window->frameGeometry();
    data->lastOutputRect = output->geometry();
}

static void updateFullShuffleState_helper(ShuffleData *data, AbstractClient *window)
{
    updateLastShuffleState_helper(data, window);
    updateSavedShuffleState_helper(data);
}

void Shuffler::updateLastShuffleState(AbstractClient *client)
{
    Q_ASSERT(m_shuffleData.contains(client));
    updateLastShuffleState_helper(&m_shuffleData[client], client);
}

void Shuffler::updateFullShuffleState(AbstractClient *client)
{
    Q_ASSERT(m_shuffleData.contains(client));
    updateFullShuffleState_helper(&m_shuffleData[client], client);
}

} // namespace KWin
