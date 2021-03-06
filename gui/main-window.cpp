/* Copyright (c) 2013-2016, Stanislaw Halik <sthalik@misaki.pl>

 * Permission to use, copy, modify, and/or distribute this
 * software for any purpose with or without fee is hereby granted,
 * provided that the above copyright notice and this permission
 * notice appear in all copies.
 */

#include "main-window.hpp"
#include "logic/tracker.h"
#include "options/options.hpp"
#include "opentrack-library-path.h"
#include "new_file_dialog.h"
#include <QFile>
#include <QFileDialog>
#include <QDesktopServices>
#include <QCoreApplication>
#include <QIcon>
#include <QString>
#include <QChar>

#ifdef _WIN32
#   include <windows.h>
#endif

extern "C" const char* opentrack_version;

MainWindow::MainWindow() :
    State(OPENTRACK_BASE_PATH + OPENTRACK_LIBRARY_PATH),
    pose_update_timer(this),
    kbd_quit(QKeySequence("Ctrl+Q"), this),
    menu_action_header(&tray_menu),
    menu_action_show(&tray_menu),
    menu_action_exit(&tray_menu),
    menu_action_tracker(&tray_menu),
    menu_action_filter(&tray_menu),
    menu_action_proto(&tray_menu),
    menu_action_options(&tray_menu),
    menu_action_mappings(&tray_menu)
{
    ui.setupUi(this);
    setFixedSize(size());
    updateButtonState(false, false);

    refresh_config_list();

    connect(ui.btnEditCurves, SIGNAL(clicked()), this, SLOT(showCurveConfiguration()));
    connect(ui.btnShortcuts, SIGNAL(clicked()), this, SLOT(show_options_dialog()));
    connect(ui.btnShowEngineControls, SIGNAL(clicked()), this, SLOT(showTrackerSettings()));
    connect(ui.btnShowServerControls, SIGNAL(clicked()), this, SLOT(showProtocolSettings()));
    connect(ui.btnShowFilterControls, SIGNAL(clicked()), this, SLOT(showFilterSettings()));
    connect(ui.btnStartTracker, SIGNAL(clicked()), this, SLOT(startTracker()));
    connect(ui.btnStopTracker, SIGNAL(clicked()), this, SLOT(stopTracker()));
    connect(ui.iconcomboProfile, SIGNAL(currentTextChanged(QString)), this, SLOT(profile_selected(QString)));

    // fill dylib comboboxen
    {
        modules.filters().push_front(std::make_shared<dylib>("", dylib::Filter));

        for (mem<dylib>& x : modules.trackers())
            ui.iconcomboTrackerSource->addItem(x->icon, x->name);

        for (mem<dylib>& x : modules.protocols())
            ui.iconcomboProtocol->addItem(x->icon, x->name);

        for (mem<dylib>& x : modules.filters())
            ui.iconcomboFilter->addItem(x->icon, x->name);
    }

    // dylibs
    {
        tie_setting(m.tracker_dll, ui.iconcomboTrackerSource);
        tie_setting(m.protocol_dll, ui.iconcomboProtocol);
        tie_setting(m.filter_dll, ui.iconcomboFilter);

        connect(ui.iconcomboTrackerSource,
                &QComboBox::currentTextChanged,
                [&](QString) -> void { if (pTrackerDialog) pTrackerDialog = nullptr; save_modules(); });

        connect(ui.iconcomboProtocol,
                &QComboBox::currentTextChanged,
                [&](QString) -> void { if (pProtocolDialog) pProtocolDialog = nullptr; save_modules(); });

        connect(ui.iconcomboFilter,
                &QComboBox::currentTextChanged,
                [&](QString) -> void { if (pFilterDialog) pFilterDialog = nullptr; save_modules(); });
    }

    // timers
    connect(&config_list_timer, SIGNAL(timeout()), this, SLOT(refresh_config_list()));
    connect(&pose_update_timer, SIGNAL(timeout()), this, SLOT(showHeadPose()));
    connect(&det_timer, SIGNAL(timeout()), this, SLOT(maybe_start_profile_from_executable()));

    // ctrl+q exits
    connect(&kbd_quit, SIGNAL(activated()), this, SLOT(exit()));

    // profile menu
    {
        profile_menu.addAction("Create new empty config", this, SLOT(make_empty_config()));
        profile_menu.addAction("Create new copied config", this, SLOT(make_copied_config()));
        profile_menu.addAction("Open configuration directory", this, SLOT(open_config_directory()));
        ui.profile_button->setMenu(&profile_menu);
    }

    if (!QFile(group::ini_pathname()).exists())
    {
        set_profile(OPENTRACK_DEFAULT_CONFIG);
        const auto pathname = group::ini_pathname();
        if (!QFile(pathname).exists())
        {
            QFile file(pathname);
            (void) file.open(QFile::ReadWrite);
        }
    }

    connect(this, &MainWindow::emit_start_tracker,
            this, [&]() -> void { qDebug() << "start tracker"; startTracker(); },
            Qt::QueuedConnection);

    connect(this, &MainWindow::emit_stop_tracker,
            this, [&]() -> void { qDebug() << "stop tracker"; stopTracker(); },
            Qt::QueuedConnection);

    connect(this, &MainWindow::emit_toggle_tracker,
            this, [&]() -> void { qDebug() << "toggle tracker"; if (work) stopTracker(); else startTracker(); },
            Qt::QueuedConnection);

    connect(this, &MainWindow::emit_restart_tracker,
            this, [&]() -> void { qDebug() << "restart tracker"; stopTracker(); startTracker(); },
            Qt::QueuedConnection);

    // tray
    {
        init_tray_menu();

        connect(&s.tray_enabled,
                static_cast<void (base_value::*)(bool) const>(&base_value::valueChanged),
                this,
                [&](bool) { ensure_tray(); });
        ensure_tray();
    }

    if (group::ini_directory() == "")
        QMessageBox::warning(this,
                             "Configuration not saved.",
                             "Can't create configuration directory! Expect major malfunction.",
                             QMessageBox::Ok, QMessageBox::NoButton);

    register_shortcuts();
    det_timer.start(1000);
    config_list_timer.start(1000 * 5);
    kbd_quit.setEnabled(true);
}

void MainWindow::init_tray_menu()
{
    tray_menu.clear();

    QString display_name(opentrack_version);
    if (display_name.startsWith("opentrack-"))
        display_name.replace(sizeof("opentrack") - 1, 1, QChar(' '));
    if (display_name.endsWith("-DEBUG"))
        display_name.replace(display_name.size() - int(sizeof("DEBUG")), display_name.size(), " (debug)");

    menu_action_header.setEnabled(false);
    menu_action_header.setText(display_name);
    menu_action_header.setIcon(QIcon(":/images/facetracknoir.png"));
    tray_menu.addAction(&menu_action_header);

    menu_action_show.setIconVisibleInMenu(true);
    menu_action_show.setText(isHidden() ? "Show the Octopus" : "Hide the Octopus");
    menu_action_show.setIcon(QIcon(":/images/facetracknoir.png"));
    QObject::connect(&menu_action_show, &QAction::triggered, this, [&]() { toggle_restore_from_tray(QSystemTrayIcon::Trigger); });
    tray_menu.addAction(&menu_action_show);

    tray_menu.addSeparator();

    menu_action_tracker.setText("Tracker settings");
    menu_action_tracker.setIcon(QIcon(":/images/tools.png"));
    QObject::connect(&menu_action_tracker, &QAction::triggered, this, &MainWindow::showTrackerSettings);
    tray_menu.addAction(&menu_action_tracker);

    menu_action_filter.setText("Filter settings");
    menu_action_filter.setIcon(QIcon(":/images/filter-16.png"));
    QObject::connect(&menu_action_filter, &QAction::triggered, this, &MainWindow::showFilterSettings);
    tray_menu.addAction(&menu_action_filter);

    menu_action_proto.setText("Protocol settings");
    menu_action_proto.setIcon(QIcon(":/images/settings16.png"));
    QObject::connect(&menu_action_proto, &QAction::triggered, this, &MainWindow::showProtocolSettings);
    tray_menu.addAction(&menu_action_proto);

    tray_menu.addSeparator();

    menu_action_mappings.setIcon(QIcon(":/images/curves.png"));
    menu_action_mappings.setText("Mappings");
    QObject::connect(&menu_action_mappings, &QAction::triggered, this, &MainWindow::showCurveConfiguration);
    tray_menu.addAction(&menu_action_mappings);

    menu_action_options.setIcon(QIcon(":/images/tools.png"));
    menu_action_options.setText("Options");
    QObject::connect(&menu_action_options, &QAction::triggered, this, &MainWindow::show_options_dialog);
    tray_menu.addAction(&menu_action_options);

    tray_menu.addSeparator();

    menu_action_exit.setText("Exit");
    QObject::connect(&menu_action_exit, &QAction::triggered, this, &MainWindow::exit);
    tray_menu.addAction(&menu_action_exit);
}

void MainWindow::register_shortcuts()
{
    using t_key = Shortcuts::t_key;
    using t_keys = Shortcuts::t_keys;

    t_keys keys
    {
        t_key(s.key_start_tracking, [&](bool) -> void { emit_start_tracker(); }, true),
        t_key(s.key_stop_tracking, [&](bool) -> void { emit_stop_tracker(); }, true),
        t_key(s.key_toggle_tracking, [&](bool) -> void { emit_toggle_tracker(); }, true),
        t_key(s.key_restart_tracking, [&](bool) -> void { emit_restart_tracker(); }, true),
    };

    global_shortcuts.reload(keys);

    if (work)
        work->reload_shortcuts();
}

void MainWindow::warn_on_config_not_writable()
{
    QString current_file = group::ini_pathname();
    QFile f(current_file);
    f.open(QFile::ReadWrite);

    if (!f.isOpen())
    {
        QMessageBox::warning(this, "Something went wrong", "Check permissions and ownership for your .ini file!", QMessageBox::Ok, QMessageBox::NoButton);
    }
}

bool MainWindow::get_new_config_name_from_dialog(QString& ret)
{
    new_file_dialog dlg;
    dlg.exec();
    return dlg.is_ok(ret);
}

MainWindow::~MainWindow()
{
    if (tray)
        tray->hide();
    stopTracker();
    save_modules();
}

void MainWindow::set_working_directory()
{
    QDir::setCurrent(OPENTRACK_BASE_PATH);
}

void MainWindow::save_modules()
{
    m.b->save();
}

void MainWindow::make_empty_config()
{
    QString name;
    const QString dir = group::ini_directory();
    if (dir != "" && get_new_config_name_from_dialog(name))
    {
        QFile filename(dir + "/" + name);
        (void) filename.open(QFile::ReadWrite);
        refresh_config_list();
        ui.iconcomboProfile->setCurrentText(name);
    }
}

void MainWindow::make_copied_config()
{
    const QString dir = group::ini_directory();
    const QString cur = group::ini_pathname();
    QString name;
    if (cur != "" && dir != "" && get_new_config_name_from_dialog(name))
    {
        const QString new_name = dir + "/" + name;
        (void) QFile::remove(new_name);
        (void) QFile::copy(cur, new_name);
        refresh_config_list();
        ui.iconcomboProfile->setCurrentText(name);
    }
}

void MainWindow::open_config_directory()
{
    const QString path = group::ini_directory();
    if (path != "")
    {
        QDesktopServices::openUrl("file:///" + QDir::toNativeSeparators(path));
    }
}

void MainWindow::refresh_config_list()
{
    if (work)
        return;

    QStringList ini_list = group::ini_list();

    if (ini_list.size() == 0)
    {
        QFile filename(group::ini_directory() + "/" OPENTRACK_DEFAULT_CONFIG);
        (void) filename.open(QFile::ReadWrite);
        ini_list.append(OPENTRACK_DEFAULT_CONFIG);
    }

    if (progn(
                if (ini_list.size() == ui.iconcomboProfile->count())
                {
                    const int sz = ini_list.size();
                    for (int i = 0; i < sz; i++)
                    {
                        if (ini_list[i] != ui.iconcomboProfile->itemText(i))
                            return false;
                    }
                    return true;
                }

                return false;
             ))
    {
        // don't even warn on non-writable.
        // it'd happen all the time since refresh is on a timer.
        return;
    }

    QString current = group::ini_filename();

    {
        inhibit_qt_signals l(*ui.iconcomboProfile);

        ui.iconcomboProfile->clear();
        ui.iconcomboProfile->addItems(ini_list);

        QIcon icon(":/images/settings16.png");

        {
            const int sz = ini_list.size();

            for (int i = 0; i < sz; i++)
                ui.iconcomboProfile->setItemIcon(i, icon);
        }

        ui.iconcomboProfile->setCurrentText(current);
    }

    set_title();
    warn_on_config_not_writable();
}

void MainWindow::updateButtonState(bool running, bool inertialp)
{
    bool not_running = !running;
    ui.iconcomboProfile->setEnabled ( not_running );
    ui.btnStartTracker->setEnabled ( not_running );
    ui.btnStopTracker->setEnabled ( running );
    ui.iconcomboProtocol->setEnabled ( not_running );
    ui.iconcomboFilter->setEnabled ( not_running );
    ui.iconcomboTrackerSource->setEnabled(not_running);
    ui.profile_button->setEnabled(not_running);
    ui.video_frame_label->setVisible(not_running || inertialp);
    if(not_running)
    {
        ui.video_frame_label->setPixmap(QPixmap(":/images/tracking-not-started.png"));
    }
    else {
        ui.video_frame_label->setPixmap(QPixmap(":/images/no-feed.png"));
    }
}

void MainWindow::startTracker()
{
    if (work)
        return;

    // tracker dtor needs run first
    work = nullptr;

    libs = SelectedLibraries(ui.video_frame, current_tracker(), current_protocol(), current_filter());

    {
        double p[6] = {0,0,0, 0,0,0};
        display_pose(p, p);
    }

    if (!libs.correct)
    {
        QMessageBox::warning(this, "Library load error",
                             "One of libraries failed to load. Check installation.",
                             QMessageBox::Ok,
                             QMessageBox::NoButton);
        libs = SelectedLibraries();
        return;
    }

    save_modules();

    work = std::make_shared<Work>(pose, libs, winId());
    work->reload_shortcuts();

    if (pTrackerDialog)
        pTrackerDialog->register_tracker(libs.pTracker.get());

    if (pFilterDialog)
        pFilterDialog->register_filter(libs.pFilter.get());

    if (pProtocolDialog)
        pProtocolDialog->register_protocol(libs.pProtocol.get());

    pose_update_timer.start(50);

    // NB check valid since SelectedLibraries ctor called
    // trackers take care of layout state updates
    const bool is_inertial = ui.video_frame->layout() == nullptr;
    updateButtonState(true, is_inertial);

    // Update the state of the options window directly.
    // Might be better to emit signals and allow the options window
    // to connect its slots to them (?)
    if (options_widget)
        options_widget->update_widgets_states(true);

    ui.btnStopTracker->setFocus();
}

void MainWindow::stopTracker()
{
    if (!work)
        return;

    //ui.game_name->setText("Not connected");

    pose_update_timer.stop();
    ui.pose_display->rotateBy_real(0, 0, 0, 0, 0, 0);

    if (pTrackerDialog)
        pTrackerDialog->unregister_tracker();

    if (pProtocolDialog)
        pProtocolDialog->unregister_protocol();

    if (pFilterDialog)
        pFilterDialog->unregister_filter();

    save_modules();

    work = nullptr;
    libs = SelectedLibraries();

    {
        double p[6] = {0,0,0, 0,0,0};
        display_pose(p, p);
    }
    updateButtonState(false, false);

    if (options_widget)
        options_widget->update_widgets_states(false);

    set_title();

    ui.btnStartTracker->setFocus();
}

void MainWindow::display_pose(const double *mapped, const double *raw)
{
    ui.pose_display->rotateBy(mapped[Yaw], mapped[Pitch], -mapped[Roll],
                              mapped[TX], mapped[TY], mapped[TZ]);

    if (mapping_widget)
        mapping_widget->update();

    double mapped_[6], raw_[6];

    for (int i = 0; i < 6; i++)
    {
        mapped_[i] = int(mapped[i]);
        raw_[i] = int(raw[i]);
    }

    ui.raw_x->display(raw_[TX]);
    ui.raw_y->display(raw_[TY]);
    ui.raw_z->display(raw_[TZ]);
    ui.raw_yaw->display(raw_[Yaw]);
    ui.raw_pitch->display(raw_[Pitch]);
    ui.raw_roll->display(raw_[Roll]);

    ui.pose_x->display(mapped_[TX]);
    ui.pose_y->display(mapped_[TY]);
    ui.pose_z->display(mapped_[TZ]);
    ui.pose_yaw->display(mapped_[Yaw]);
    ui.pose_pitch->display(mapped_[Pitch]);
    ui.pose_roll->display(mapped_[Roll]);

    QString game_title;
    if (libs.pProtocol)
        game_title = libs.pProtocol->game_name();
    set_title(game_title);
}

void MainWindow::set_title(const QString& game_title_)
{
    QString game_title;
    if (game_title_ != "")
        game_title = " :: " + game_title_;
    QString current = group::ini_filename();
    setWindowTitle(opentrack_version + QStringLiteral(" :: ") + current + game_title);
}

void MainWindow::showHeadPose()
{
    double mapped[6], raw[6];

    work->tracker->get_raw_and_mapped_poses(mapped, raw);

    display_pose(mapped, raw);
}

template<typename t>
bool mk_dialog(mem<dylib> lib, ptr<t>& orig)
{
    if (orig && orig->isVisible())
    {
        orig->show();
        orig->raise();
        return false;
    }

    if (lib && lib->Dialog)
    {
        t* dialog = reinterpret_cast<t*>(lib->Dialog());
        dialog->setWindowFlags(Qt::Dialog);
        dialog->setFixedSize(dialog->size());

        orig.reset(dialog);
        dialog->show();

        QObject::connect(dialog, &plugin_api::detail::BaseDialog::closing, [&]() -> void { orig = nullptr; });

        return true;
    }

    return false;
}

void MainWindow::showTrackerSettings()
{
    if (mk_dialog(current_tracker(), pTrackerDialog) && libs.pTracker)
        pTrackerDialog->register_tracker(libs.pTracker.get());
}

void MainWindow::showProtocolSettings()
{
    if (mk_dialog(current_protocol(), pProtocolDialog) && libs.pProtocol)
        pProtocolDialog->register_protocol(libs.pProtocol.get());
}

void MainWindow::showFilterSettings()
{
    if (mk_dialog(current_filter(), pFilterDialog) && libs.pFilter)
        pFilterDialog->register_filter(libs.pFilter.get());
}

template<typename t, typename... Args>
static bool mk_window(ptr<t>* place, Args&&... params)
{
    if (*place && (*place)->isVisible())
    {
        (*place)->show();
        (*place)->raise();
        return false;
    }
    else
    {
        *place = make_unique<t>(std::forward<Args>(params)...);
        (*place)->setWindowFlags(Qt::Dialog);
        (*place)->show();
        return true;
    }
}

void MainWindow::show_options_dialog()
{
    if (mk_window(&options_widget, [&](bool flag) -> void { set_keys_enabled(!flag); }))
    {
        connect(options_widget.get(), &OptionsDialog::closing, this, &MainWindow::register_shortcuts);
        options_widget->update_widgets_states(work != nullptr);
    }
}

void MainWindow::showCurveConfiguration()
{
    mk_window(&mapping_widget, pose);
}

void MainWindow::exit()
{
    QCoreApplication::exit(0);
}

void MainWindow::profile_selected(const QString& name)
{
    const auto old_name = group::ini_filename();
    const auto new_name = name;

    if (name == "")
        return;

    if (old_name != new_name)
    {
        save_modules();
        set_profile(new_name);
        set_title();
        options::detail::bundler::refresh_all_bundles();
    }
}

void MainWindow::ensure_tray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;

    if (s.tray_enabled)
    {
        if (!tray)
        {
            tray = make_unique<QSystemTrayIcon>(this);
            tray->setIcon(QIcon(":/images/facetracknoir.png"));
            tray->setContextMenu(&tray_menu);
            tray->show();

            connect(tray.get(),
                    &QSystemTrayIcon::activated,
                    this,
                    &MainWindow::toggle_restore_from_tray);
        }
    }
    else
    {
        if (isHidden())
            show();
        if (!isVisible())
            setVisible(true);

        raise(); // for OSX
        activateWindow(); // for Windows

        if (tray)
            tray->hide();
        tray = nullptr;
    }
}

void MainWindow::toggle_restore_from_tray(QSystemTrayIcon::ActivationReason e)
{
    if (progn(switch (e)
              {
              // if we enable double click also then it causes
              // toggle back to the original state
              //case QSystemTrayIcon::DoubleClick:
              case QSystemTrayIcon::Trigger: // single click
                  return false;
              default:
                  return true;
              }))
        return;

    ensure_tray();

    const bool is_minimized = isHidden() || !is_tray_enabled();

    menu_action_show.setText(!isHidden() ? "Show the Octopus" : "Hide the Octopus");

    setVisible(is_minimized);
    setHidden(!is_minimized);

    setWindowState(progn(
                       using ws = Qt::WindowStates;
                       if (is_minimized)
                           return ws(windowState() & (~Qt::WindowMinimized));
                       else
                           return ws(Qt::WindowNoState);
                       ));

    if (is_minimized)
    {
        raise(); // for OSX
        activateWindow(); // for Windows
    }
    else
    {
        lower();
        clearFocus();
    }
}

bool MainWindow::maybe_hide_to_tray(QEvent* e)
{
    if (e->type() == QEvent::WindowStateChange &&
        (windowState() & Qt::WindowMinimized) &&
        is_tray_enabled())
    {
        e->accept();
        ensure_tray();
        hide();

        return true;
    }

    return false;
}

void MainWindow::maybe_start_profile_from_executable()
{
    if (!work)
    {
        QString prof;
        if (det.config_to_start(prof))
        {
            ui.iconcomboProfile->setCurrentText(prof);
            startTracker();
        }
    }
    else
    {
        if (det.should_stop())
            stopTracker();
    }
}

void MainWindow::set_keys_enabled(bool flag)
{
    if (!flag)
    {
        if (work)
            work->sc->reload({});
        global_shortcuts.reload({});
    }
    else
    {
        register_shortcuts();
    }
    qDebug() << "keybindings set to" << flag;
}

void MainWindow::changeEvent(QEvent* e)
{
    if (maybe_hide_to_tray(e))
        e->accept();
    else
    {
        QMainWindow::changeEvent(e);
    }
}

void MainWindow::closeEvent(QCloseEvent*)
{
    exit();
}

bool MainWindow::is_tray_enabled()
{
    return s.tray_enabled && QSystemTrayIcon::isSystemTrayAvailable();
}

void MainWindow::set_profile(const QString &profile)
{
    QSettings settings(OPENTRACK_ORG);
    settings.setValue(OPENTRACK_CONFIG_FILENAME_KEY, profile);
    warn_on_config_not_writable();
}
