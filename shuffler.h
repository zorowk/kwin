/*
    SPDX-FileCopyrightText: 2020 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QHash>
#include <QObject>
#include <QRect>
#include <QString>

namespace KWin
{

class AbstractClient;

/**
 * The ShuffleData struct keeps data necessary to determine where a window has to be moved
 * after an output has been connected or disconnected.
 *
 * The ShuffleData contains two states - the saved state and the last state. If the screen
 * layout has changed and the window has not been moved before due to output changes, the
 * information about the original output will be stored in both saved and last state. If
 * another output change occurs and the original output hasn't come back online, only the
 * last state is going to be updated.
 *
 * It might seem like the last state is redundant, but without it, we can't reliably determine
 * the last output for a window because outputs have changed their position.
 */
struct ShuffleData
{
    QRect savedOutputRect;
    QRect savedGeometry;
    QString savedOutputName;

    QRect lastOutputRect;
    QRect lastGeometry;
    QString lastOutputName;
};

/**
 * The Shuffler class tries to move windows back to their original screens.
 *
 * The Shuffler class keeps a record for every window where it has been last seen. If the
 * output where the window is on had been disconnected and later on connected back, the
 * window shuffler will try to put the window back on the original output. However, there
 * are multiple reasons why the window might not be restored, e.g. it has been moved on a
 * new output by user, etc.
 */
class Shuffler : public QObject
{
    Q_OBJECT

public:
    explicit Shuffler(QObject *parent = nullptr);

    void shuffle();

private Q_SLOTS:
    void handleClientAdded(AbstractClient *client);
    void handleClientRemoved(AbstractClient *client);

private:
    void updateLastShuffleState(AbstractClient *client);
    void updateFullShuffleState(AbstractClient *client);

    QHash<AbstractClient *, ShuffleData> m_shuffleData;
};

} // namespace KWin
