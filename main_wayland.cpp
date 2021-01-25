/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2014 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "main_wayland.h"
#include "composite.h"
#include "inputmethod.h"
#include "workspace.h"
#include <config-kwin.h>
// kwin
#include "platform.h"
#include "effects.h"
#include "tabletmodemanager.h"

#include "wayland_server.h"
#include "xwl/xwayland.h"

// KWayland
#include <KWaylandServer/display.h>
#include <KWaylandServer/seat_interface.h>

// KDE
#include <KCrash>
#include <KDesktopFile>
#include <KLocalizedString>
#include <KPluginLoader>
#include <KPluginMetaData>
#include <KQuickAddons/QtQuickSettings>
#include <KShell>

// Qt
#include <qplatformdefs.h>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QProcess>
#include <QStyle>
#include <QDebug>
#include <QWindow>

// system
#if HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif
#if HAVE_SYS_PROCCTL_H
#include <sys/procctl.h>
#endif

#if HAVE_LIBCAP
#include <sys/capability.h>
#endif

#include <sched.h>

#include <iostream>
#include <iomanip>

Q_IMPORT_PLUGIN(KWinIntegrationPlugin)
Q_IMPORT_PLUGIN(KGlobalAccelImpl)
Q_IMPORT_PLUGIN(KWindowSystemKWinPlugin)
Q_IMPORT_PLUGIN(KWinIdleTimePoller)
#ifdef PipeWire_FOUND
Q_IMPORT_PLUGIN(ScreencastManagerFactory)
#endif

namespace KWin
{

static void sighandler(int)
{
    QApplication::exit();
}

void disableDrKonqi()
{
    KCrash::setDrKonqiEnabled(false);
}
// run immediately, before Q_CORE_STARTUP functions
// that would enable drkonqi
Q_CONSTRUCTOR_FUNCTION(disableDrKonqi)

enum class RealTimeFlags
{
    DontReset,
    ResetOnFork
};

namespace {
void gainRealTime(RealTimeFlags flags = RealTimeFlags::DontReset)
{
#if HAVE_SCHED_RESET_ON_FORK
    const int minPriority = sched_get_priority_min(SCHED_RR);
    struct sched_param sp;
    sp.sched_priority = minPriority;
    int policy = SCHED_RR;
    if (flags == RealTimeFlags::ResetOnFork) {
        policy |= SCHED_RESET_ON_FORK;
    }
    sched_setscheduler(0, policy, &sp);
#else
    Q_UNUSED(flags);
#endif
}
}

//************************************
// ApplicationWayland
//************************************

ApplicationWayland::ApplicationWayland(int &argc, char **argv)
    : ApplicationWaylandAbstract(OperationModeWaylandOnly, argc, argv)
{
    // Stop restarting the input method if it starts crashing very frequently
    m_inputMethodCrashTimer.setInterval(20000);
    m_inputMethodCrashTimer.setSingleShot(true);
    connect(&m_inputMethodCrashTimer, &QTimer::timeout, this, [this] {
        m_inputMethodCrashes = 0;
    });
    connect(waylandServer(), &WaylandServer::terminatingInternalClientConnection, this, &ApplicationWayland::stopInputMethod);
}

ApplicationWayland::~ApplicationWayland()
{
    setTerminating();
    stopInputMethod();
    if (!waylandServer()) {
        return;
    }

    if (auto *platform = kwinApp()->platform()) {
        platform->prepareShutdown();
    }
    // need to unload all effects prior to destroying X connection as they might do X calls
    if (effects) {
        static_cast<EffectsHandlerImpl*>(effects)->unloadAllEffects();
    }
    destroyPlugins();
    delete m_xwayland;
    m_xwayland = nullptr;
    destroyWorkspace();
    waylandServer()->dispatch();

    if (QStyle *s = style()) {
        s->unpolish(this);
    }
    waylandServer()->terminateClientConnections();
    destroyCompositor();
    destroyColorManager();
}

void ApplicationWayland::performStartup()
{
    if (m_startXWayland) {
        setOperationMode(OperationModeXwayland);
    }
    // first load options - done internally by a different thread
    createOptions();
    createSession();
    createColorManager();
    waylandServer()->createInternalConnection();

    // try creating the Wayland Backend
    createInput();
    // now libinput thread has been created, adjust scheduler to not leak into other processes
    gainRealTime(RealTimeFlags::ResetOnFork);

    InputMethod::create(this);
    createBackend();
    TabletModeManager::create(this);
    createPlugins();
}

void ApplicationWayland::createBackend()
{
    connect(platform(), &Platform::screensQueried, this, &ApplicationWayland::continueStartupWithScreens);
    connect(platform(), &Platform::initFailed, this,
        [] () {
            std::cerr <<  "FATAL ERROR: backend failed to initialize, exiting now" << std::endl;
            QCoreApplication::exit(1);
        }
    );
    platform()->init();
}

void ApplicationWayland::continueStartupWithScreens()
{
    disconnect(kwinApp()->platform(), &Platform::screensQueried, this, &ApplicationWayland::continueStartupWithScreens);
    createScreens();
    WaylandCompositor::create();
    connect(Compositor::self(), &Compositor::sceneCreated, this, &ApplicationWayland::continueStartupWithScene);
}

void ApplicationWayland::finalizeStartup()
{
    if (m_xwayland) {
        disconnect(m_xwayland, &Xwl::Xwayland::errorOccurred, this, &ApplicationWayland::finalizeStartup);
        disconnect(m_xwayland, &Xwl::Xwayland::started, this, &ApplicationWayland::finalizeStartup);
    }
    startSession();
    notifyStarted();
}

void ApplicationWayland::continueStartupWithScene()
{
    disconnect(Compositor::self(), &Compositor::sceneCreated, this, &ApplicationWayland::continueStartupWithScene);

    // Note that we start accepting client connections after creating the Workspace.
    createWorkspace();

    if (!waylandServer()->start()) {
        qFatal("Failed to initialze the Wayland server, exiting now");
    }

    if (operationMode() == OperationModeWaylandOnly) {
        finalizeStartup();
        return;
    }

    m_xwayland = new Xwl::Xwayland(this);
    connect(m_xwayland, &Xwl::Xwayland::errorOccurred, this, &ApplicationWayland::finalizeStartup);
    connect(m_xwayland, &Xwl::Xwayland::started, this, &ApplicationWayland::finalizeStartup);
    m_xwayland->start();
}


void ApplicationWayland::stopInputMethod()
{
    if (!m_inputMethodProcess) {
        return;
    }
    m_inputMethodProcess->kill();
    m_inputMethodProcess->waitForFinished();
    if (waylandServer()) {
        waylandServer()->destroyInputMethodConnection();
    }
    m_inputMethodProcess->deleteLater();
    m_inputMethodProcess = nullptr;
}

void ApplicationWayland::startInputMethod(const QString &executable)
{
    stopInputMethod();
    if (executable.isEmpty() || isTerminating()) {
        return;
    }

    QStringList arguments = KShell::splitArgs(executable);
    if (arguments.isEmpty()) {
        qWarning("Failed to launch the input method server: %s is an invalid command", qPrintable(m_inputMethodServerToStart));
        return;
    }

    const QString program = arguments.takeFirst();
    int socket = dup(waylandServer()->createInputMethodConnection());
    if (socket < 0) {
        qWarning("Failed to create the input method connection");
        return;
    }

    QProcessEnvironment environment = processStartupEnvironment();
    environment.insert(QStringLiteral("WAYLAND_SOCKET"), QByteArray::number(socket));
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("wayland"));
    environment.remove("DISPLAY");
    environment.remove("WAYLAND_DISPLAY");

    m_inputMethodProcess = new Process(this);
    m_inputMethodProcess->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    m_inputMethodProcess->setProcessEnvironment(environment);
    m_inputMethodProcess->setProgram(program);
    m_inputMethodProcess->setArguments(arguments);
    m_inputMethodProcess->start();
    connect(m_inputMethodProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this, executable] (int exitCode, QProcess::ExitStatus exitStatus) {
        if (exitStatus == QProcess::CrashExit) {
            m_inputMethodCrashes++;
            m_inputMethodCrashTimer.start();
            qWarning() << "Input Method crashed" << executable << exitCode << exitStatus;
            if (m_inputMethodCrashes < 5) {
                startInputMethod(executable);
            } else {
                qWarning() << "Input Method keeps crashing, please fix" << executable;
                stopInputMethod();
            }
        }
    });
}

void ApplicationWayland::refreshSettings(const KConfigGroup &group, const QByteArrayList &names)
{
    if (group.name() != "Wayland" || !names.contains("InputMethod")) {
        return;
    }

    startInputMethod(group.readEntry("InputMethod", QString()));
}

void ApplicationWayland::startSession()
{
    if (!m_inputMethodServerToStart.isEmpty()) {
        startInputMethod(m_inputMethodServerToStart);
    } else {
        KSharedConfig::Ptr kwinSettings = kwinApp()->config();
        m_settingsWatcher = KConfigWatcher::create(kwinSettings);
        connect(m_settingsWatcher.data(), &KConfigWatcher::configChanged, this, &ApplicationWayland::refreshSettings);

        KConfigGroup group = kwinSettings->group("Wayland");
        KDesktopFile file(group.readEntry("InputMethod", QString()));
        startInputMethod(file.desktopGroup().readEntry("Exec", QString()));
    }

    // start session
    if (!m_sessionArgument.isEmpty()) {
        QStringList arguments = KShell::splitArgs(m_sessionArgument);
        if (!arguments.isEmpty()) {
            QString program = arguments.takeFirst();
            QProcess *p = new Process(this);
            p->setProcessChannelMode(QProcess::ForwardedErrorChannel);
            p->setProcessEnvironment(processStartupEnvironment());
            connect(p, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [p] (int code, QProcess::ExitStatus status) {
                p->deleteLater();
                if (status == QProcess::CrashExit) {
                    qWarning() << "Session process has crashed";
                    QCoreApplication::exit(-1);
                    return;
                }

                if (code) {
                    qWarning() << "Session process exited with code" << code;
                }

                QCoreApplication::exit(code);
            });
            p->setProgram(program);
            p->setArguments(arguments);
            p->start();
        } else {
            qWarning("Failed to launch the session process: %s is an invalid command",
                     qPrintable(m_sessionArgument));
        }
    }
    // start the applications passed to us as command line arguments
    if (!m_applicationsToStart.isEmpty()) {
        for (const QString &application: m_applicationsToStart) {
            QStringList arguments = KShell::splitArgs(application);
            if (arguments.isEmpty()) {
                qWarning("Failed to launch application: %s is an invalid command",
                         qPrintable(application));
                continue;
            }
            QString program = arguments.takeFirst();
            // note: this will kill the started process when we exit
            // this is going to happen anyway as we are the wayland and X server the app connects to
            QProcess *p = new Process(this);
            p->setProcessChannelMode(QProcess::ForwardedErrorChannel);
            p->setProcessEnvironment(processStartupEnvironment());
            p->setProgram(program);
            p->setArguments(arguments);
            p->startDetached();
            p->deleteLater();
        }
    }
}

static const QString s_waylandPlugin = QStringLiteral("KWinWaylandWaylandBackend");
static const QString s_x11Plugin = QStringLiteral("KWinWaylandX11Backend");
static const QString s_fbdevPlugin = QStringLiteral("KWinWaylandFbdevBackend");
#if HAVE_DRM
static const QString s_drmPlugin = QStringLiteral("KWinWaylandDrmBackend");
#endif
static const QString s_virtualPlugin = QStringLiteral("KWinWaylandVirtualBackend");


enum SpawnMode {
    Standalone,
    ReusedSocket
};

static QString automaticBackendSelection(SpawnMode spawnMode)
{
    /* WAYLAND_DISPLAY is set by the kwin_wayland_wrapper, so we can't use it for automatic detection.
    * If kwin_wayland_wrapper is used nested on wayland, we won't be in this path as
    * it explicitly sets '--socket' which means a backend is set and we won't be in this path anyway
    */
    if (qEnvironmentVariableIsSet("WAYLAND_DISPLAY") && spawnMode == Standalone) {
        return s_waylandPlugin;
    }
    if (qEnvironmentVariableIsSet("DISPLAY")) {
        return s_x11Plugin;
    }
#if HAVE_DRM
    return s_drmPlugin;
#endif
    return s_fbdevPlugin;
}

static void disablePtrace()
{
#if HAVE_PR_SET_DUMPABLE
    // check whether we are running under a debugger
    const QFileInfo parent(QStringLiteral("/proc/%1/exe").arg(getppid()));
    if (parent.isSymLink() &&
            (parent.symLinkTarget().endsWith(QLatin1String("/gdb")) ||
             parent.symLinkTarget().endsWith(QLatin1String("/gdbserver")) ||
             parent.symLinkTarget().endsWith(QLatin1String("/lldb-server")))) {
        // debugger, don't adjust
        return;
    }

    // disable ptrace in kwin_wayland
    prctl(PR_SET_DUMPABLE, 0);
#endif
#if HAVE_PROC_TRACE_CTL
    // FreeBSD's rudimentary procfs does not support /proc/<pid>/exe
    // We could use the P_TRACED flag of the process to find out
    // if the process is being debugged ond FreeBSD.
    int mode = PROC_TRACE_CTL_DISABLE;
    procctl(P_PID, getpid(), PROC_TRACE_CTL, &mode);
#endif

}

static void unsetDumpable(int sig)
{
#if HAVE_PR_SET_DUMPABLE
    prctl(PR_SET_DUMPABLE, 1);
#endif
    signal(sig, SIG_IGN);
    raise(sig);
    return;
}

void dropNiceCapability()
{
#if HAVE_LIBCAP
    cap_t caps = cap_get_proc();
    if (!caps) {
        return;
    }
    cap_value_t capList[] = { CAP_SYS_NICE };
    if (cap_set_flag(caps, CAP_PERMITTED, 1, capList, CAP_CLEAR) == -1) {
        cap_free(caps);
        return;
    }
    if (cap_set_flag(caps, CAP_EFFECTIVE, 1, capList, CAP_CLEAR) == -1) {
        cap_free(caps);
        return;
    }
    cap_set_proc(caps);
    cap_free(caps);
#endif
}

} // namespace

int main(int argc, char * argv[])
{
    if (getuid() == 0) {
        std::cerr << "kwin_wayland does not support running as root." << std::endl;
        return 1;
    }
    KWin::disablePtrace();
    KWin::Application::setupMalloc();
    KWin::Application::setupLocalizedString();
    KWin::gainRealTime();
    KWin::dropNiceCapability();

    if (signal(SIGTERM, KWin::sighandler) == SIG_IGN)
        signal(SIGTERM, SIG_IGN);
    if (signal(SIGINT, KWin::sighandler) == SIG_IGN)
        signal(SIGINT, SIG_IGN);
    if (signal(SIGHUP, KWin::sighandler) == SIG_IGN)
        signal(SIGHUP, SIG_IGN);
    signal(SIGABRT, KWin::unsetDumpable);
    signal(SIGSEGV, KWin::unsetDumpable);
    signal(SIGPIPE, SIG_IGN);
    // ensure that no thread takes SIGUSR
    sigset_t userSignals;
    sigemptyset(&userSignals);
    sigaddset(&userSignals, SIGUSR1);
    sigaddset(&userSignals, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &userSignals, nullptr);

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();

    // enforce our internal qpa plugin, unfortunately command line switch has precedence
    setenv("QT_QPA_PLATFORM", "wayland-org.kde.kwin.qpa", true);

    qunsetenv("QT_DEVICE_PIXEL_RATIO");
    qputenv("QT_IM_MODULE", "qtvirtualkeyboard");
    qputenv("QSG_RENDER_LOOP", "basic");
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    KWin::ApplicationWayland a(argc, argv);
    a.setupTranslator();
    // reset QT_QPA_PLATFORM to a sane value for any processes started from KWin
    setenv("QT_QPA_PLATFORM", "wayland", true);

    KWin::Application::createAboutData();
    KQuickAddons::QtQuickSettings::init();

    const auto availablePlugins = KPluginLoader::findPlugins(QStringLiteral("org.kde.kwin.waylandbackends"));
    auto hasPlugin = [&availablePlugins] (const QString &name) {
        return std::any_of(availablePlugins.begin(), availablePlugins.end(),
            [name] (const KPluginMetaData &plugin) {
                return plugin.pluginId() == name;
            }
        );
    };
    const bool hasSizeOption = hasPlugin(KWin::s_x11Plugin) || hasPlugin(KWin::s_virtualPlugin);
    const bool hasOutputCountOption = hasPlugin(KWin::s_x11Plugin);
    const bool hasX11Option = hasPlugin(KWin::s_x11Plugin);
    const bool hasVirtualOption = hasPlugin(KWin::s_virtualPlugin);
    const bool hasWaylandOption = hasPlugin(KWin::s_waylandPlugin);
    const bool hasFramebufferOption = hasPlugin(KWin::s_fbdevPlugin);
#if HAVE_DRM
    const bool hasDrmOption = hasPlugin(KWin::s_drmPlugin);
#endif

    QCommandLineOption xwaylandOption(QStringLiteral("xwayland"),
                                      i18n("Start a rootless Xwayland server."));
    QCommandLineOption waylandSocketOption(QStringList{QStringLiteral("s"), QStringLiteral("socket")},
                                           i18n("Name of the Wayland socket to listen on. If not set \"wayland-0\" is used."),
                                           QStringLiteral("socket"));
    QCommandLineOption framebufferOption(QStringLiteral("framebuffer"),
                                         i18n("Render to framebuffer."));
    QCommandLineOption framebufferDeviceOption(QStringLiteral("fb-device"),
                                               i18n("The framebuffer device to render to."),
                                               QStringLiteral("fbdev"));
    QCommandLineOption x11DisplayOption(QStringLiteral("x11-display"),
                                        i18n("The X11 Display to use in windowed mode on platform X11."),
                                        QStringLiteral("display"));
    QCommandLineOption waylandDisplayOption(QStringLiteral("wayland-display"),
                                            i18n("The Wayland Display to use in windowed mode on platform Wayland."),
                                            QStringLiteral("display"));
    QCommandLineOption virtualFbOption(QStringLiteral("virtual"), i18n("Render to a virtual framebuffer."));
    QCommandLineOption widthOption(QStringLiteral("width"),
                                   i18n("The width for windowed mode. Default width is 1024."),
                                   QStringLiteral("width"));
    widthOption.setDefaultValue(QString::number(1024));
    QCommandLineOption heightOption(QStringLiteral("height"),
                                    i18n("The height for windowed mode. Default height is 768."),
                                    QStringLiteral("height"));
    heightOption.setDefaultValue(QString::number(768));

    QCommandLineOption scaleOption(QStringLiteral("scale"),
                                    i18n("The scale for windowed mode. Default value is 1."),
                                    QStringLiteral("scale"));
    scaleOption.setDefaultValue(QString::number(1));

    QCommandLineOption outputCountOption(QStringLiteral("output-count"),
                                    i18n("The number of windows to open as outputs in windowed mode. Default value is 1"),
                                    QStringLiteral("count"));
    outputCountOption.setDefaultValue(QString::number(1));

    QCommandLineOption waylandSocketFdOption(QStringLiteral("wayland_fd"),
                                    i18n("Wayland socket to use for incoming connections."),
                                    QStringLiteral("wayland_fd"));

    QCommandLineParser parser;
    a.setupCommandLine(&parser);
    parser.addOption(xwaylandOption);
    parser.addOption(waylandSocketOption);
    parser.addOption(waylandSocketFdOption);

    if (hasX11Option) {
        parser.addOption(x11DisplayOption);
    }
    if (hasWaylandOption) {
        parser.addOption(waylandDisplayOption);
    }
    if (hasFramebufferOption) {
        parser.addOption(framebufferOption);
        parser.addOption(framebufferDeviceOption);
    }
    if (hasVirtualOption) {
        parser.addOption(virtualFbOption);
    }
    if (hasSizeOption) {
        parser.addOption(widthOption);
        parser.addOption(heightOption);
        parser.addOption(scaleOption);
    }
    if (hasOutputCountOption) {
        parser.addOption(outputCountOption);
    }
    QCommandLineOption libinputOption(QStringLiteral("libinput"),
                                      i18n("Enable libinput support for input events processing. Note: never use in a nested session.	(deprecated)"));
    parser.addOption(libinputOption);
#if HAVE_DRM
    QCommandLineOption drmOption(QStringLiteral("drm"), i18n("Render through drm node."));
    if (hasDrmOption) {
        parser.addOption(drmOption);
    }
#endif

    QCommandLineOption inputMethodOption(QStringLiteral("inputmethod"),
                                         i18n("Input method that KWin starts."),
                                         QStringLiteral("path/to/imserver"));
    parser.addOption(inputMethodOption);

    QCommandLineOption listBackendsOption(QStringLiteral("list-backends"),
                                           i18n("List all available backends and quit."));
    parser.addOption(listBackendsOption);

    QCommandLineOption screenLockerOption(QStringLiteral("lockscreen"),
                                          i18n("Starts the session in locked mode."));
    parser.addOption(screenLockerOption);

    QCommandLineOption noScreenLockerOption(QStringLiteral("no-lockscreen"),
                                            i18n("Starts the session without lock screen support."));
    parser.addOption(noScreenLockerOption);

    QCommandLineOption noGlobalShortcutsOption(QStringLiteral("no-global-shortcuts"),
                                               i18n("Starts the session without global shortcuts support."));
    parser.addOption(noGlobalShortcutsOption);

    QCommandLineOption exitWithSessionOption(QStringLiteral("exit-with-session"),
                                             i18n("Exit after the session application, which is started by KWin, closed."),
                                             QStringLiteral("/path/to/session"));
    parser.addOption(exitWithSessionOption);

    parser.addPositionalArgument(QStringLiteral("applications"),
                                 i18n("Applications to start once Wayland and Xwayland server are started"),
                                 QStringLiteral("[/path/to/application...]"));

    parser.process(a);
    a.processCommandLine(&parser);

#ifdef KWIN_BUILD_ACTIVITIES
    a.setUseKActivities(false);
#endif

    if (parser.isSet(listBackendsOption)) {
        for (const auto &plugin: availablePlugins) {
            std::cout << std::setw(40) << std::left << qPrintable(plugin.name()) << qPrintable(plugin.description()) << std::endl;
        }
        return 0;
    }

    if (parser.isSet(exitWithSessionOption)) {
        a.setSessionArgument(parser.value(exitWithSessionOption));
    }

    KWin::Application::setUseLibinput(parser.isSet(libinputOption));

    QString pluginName;
    QSize initialWindowSize;
    QByteArray deviceIdentifier;
    int outputCount = 1;
    qreal outputScale = 1;

#if HAVE_DRM
    if (hasDrmOption && parser.isSet(drmOption)) {
        pluginName = KWin::s_drmPlugin;
    }
#endif

    if (hasSizeOption) {
        bool ok = false;
        const int width = parser.value(widthOption).toInt(&ok);
        if (!ok) {
            std::cerr << "FATAL ERROR incorrect value for width" << std::endl;
            return 1;
        }
        const int height = parser.value(heightOption).toInt(&ok);
        if (!ok) {
            std::cerr << "FATAL ERROR incorrect value for height" << std::endl;
            return 1;
        }
        const qreal scale = parser.value(scaleOption).toDouble(&ok);
        if (!ok || scale <= 0) {
            std::cerr << "FATAL ERROR incorrect value for scale" << std::endl;
            return 1;
        }

        outputScale = scale;
        initialWindowSize = QSize(width, height);
    }

    if (hasOutputCountOption) {
        bool ok = false;
        const int count = parser.value(outputCountOption).toInt(&ok);
        if (ok) {
            outputCount = qMax(1, count);
        }
    }

    if (hasX11Option && parser.isSet(x11DisplayOption)) {
        deviceIdentifier = parser.value(x11DisplayOption).toUtf8();
        pluginName = KWin::s_x11Plugin;
    } else if (hasWaylandOption && parser.isSet(waylandDisplayOption)) {
        deviceIdentifier = parser.value(waylandDisplayOption).toUtf8();
        pluginName = KWin::s_waylandPlugin;
    }

    if (hasFramebufferOption && parser.isSet(framebufferOption)) {
        pluginName = KWin::s_fbdevPlugin;
        deviceIdentifier = parser.value(framebufferDeviceOption).toUtf8();
    }
    if (hasVirtualOption && parser.isSet(virtualFbOption)) {
        pluginName = KWin::s_virtualPlugin;
    }

    if (pluginName.isEmpty()) {
        std::cerr << "No backend specified through command line argument, trying auto resolution" << std::endl;
        pluginName = KWin::automaticBackendSelection(parser.isSet(waylandSocketFdOption) ? KWin::ReusedSocket : KWin::Standalone);
    }

    auto pluginIt = std::find_if(availablePlugins.begin(), availablePlugins.end(),
        [&pluginName] (const KPluginMetaData &plugin) {
            return plugin.pluginId() == pluginName;
        }
    );
    if (pluginIt == availablePlugins.end()) {
        std::cerr << "FATAL ERROR: could not find a backend" << std::endl;
        return 1;
    }

    // TODO: create backend without having the server running
    KWin::WaylandServer *server = KWin::WaylandServer::create(&a);

    KWin::WaylandServer::InitializationFlags flags;
    if (parser.isSet(screenLockerOption)) {
        flags = KWin::WaylandServer::InitializationFlag::LockScreen;
    } else if (parser.isSet(noScreenLockerOption)) {
        flags = KWin::WaylandServer::InitializationFlag::NoLockScreenIntegration;
    }
    if (parser.isSet(noGlobalShortcutsOption)) {
        flags |= KWin::WaylandServer::InitializationFlag::NoGlobalShortcuts;
    }


    if (parser.isSet(waylandSocketFdOption)) {
        bool ok;
        int fd = parser.value(waylandSocketFdOption).toInt(&ok);
        if (ok ) {
            // make sure we don't leak this FD to children
            fcntl(fd, F_SETFD, O_CLOEXEC);
            server->display()->addSocketFileDescriptor(fd);
        } else {
            std::cerr << "FATAL ERROR: could not parse socket FD" << std::endl;
            return 1;
        }
    } else {
        const QString socketName = parser.value(waylandSocketOption);
        // being empty is fine here, addSocketName will automatically pick one
        if (!server->display()->addSocketName(socketName)) {
            std::cerr << "FATAL ERROR: could not add wayland socket " << qPrintable(socketName) << std::endl;
            return 1;
        }
    }

    if (!server->init(flags)) {
        std::cerr << "FATAL ERROR: could not create Wayland server" << std::endl;
        return 1;
    }

    a.initPlatform(*pluginIt);
    if (!a.platform()) {
        std::cerr << "FATAL ERROR: could not instantiate a backend" << std::endl;
        return 1;
    }
    if (!deviceIdentifier.isEmpty()) {
        a.platform()->setDeviceIdentifier(deviceIdentifier);
    }
    if (initialWindowSize.isValid()) {
        a.platform()->setInitialWindowSize(initialWindowSize);
    }
    a.platform()->setInitialOutputScale(outputScale);
    a.platform()->setInitialOutputCount(outputCount);

    QObject::connect(&a, &KWin::Application::workspaceCreated, server, &KWin::WaylandServer::initWorkspace);
    if (!server->socketName().isEmpty()) {
        environment.insert(QStringLiteral("WAYLAND_DISPLAY"), server->socketName());
    }
    a.setProcessStartupEnvironment(environment);
    a.setStartXwayland(parser.isSet(xwaylandOption));
    a.setApplicationsToStart(parser.positionalArguments());
    a.setInputMethodServerToStart(parser.value(inputMethodOption));
    a.start();

    return a.exec();
}
