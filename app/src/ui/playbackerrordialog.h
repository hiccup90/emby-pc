#pragma once

#include <QDialog>

class QLabel;
class QPushButton;

namespace ui {

// T64：B1-A 播放错误统一对话框。调用方只依赖 ui/playbackerrordialog.h，
// 不直接依赖 app/src/mainwindow.cpp。
class PlaybackErrorDialog : public QDialog
{
    Q_OBJECT
public:
    enum class Action {
        None,
        Retry,
        Back,
        Export,
    };

    struct Info
    {
        QString code;
        QString title;
        QString detail;
        QString diagnostics;
        bool retryEnabled = true;
    };

    explicit PlaybackErrorDialog(const Info &info, QWidget *parent = nullptr);

    Action action() const { return m_action; }
    static Action show(QWidget *parent, const Info &info);

private:
    void exportDiagnostics();

    Info m_info;
    Action m_action = Action::None;
    QLabel *m_codeLabel = nullptr;
    QLabel *m_detailLabel = nullptr;
    QPushButton *m_retryButton = nullptr;
    QPushButton *m_backButton = nullptr;
    QPushButton *m_exportButton = nullptr;
};

} // namespace ui
