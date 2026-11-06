#include "playbackerrordialog.h"

#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTextStream>
#include <QVBoxLayout>


namespace ui {

PlaybackErrorDialog::PlaybackErrorDialog(const Info &info, QWidget *parent)
    : QDialog(parent)
    , m_info(info)
{
    setObjectName(QStringLiteral("PlaybackErrorDialog"));
    setWindowTitle(QStringLiteral("播放失败"));
    setModal(true);
    setMinimumWidth(460);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 20);
    root->setSpacing(10);

    auto *title = new QLabel(info.title.isEmpty() ? QStringLiteral("播放失败") : info.title, this);
    title->setObjectName(QStringLiteral("PlaybackErrorTitle"));
    QFont titleFont = title->font();
    titleFont.setPixelSize(20);
    titleFont.setWeight(QFont::DemiBold);
    title->setFont(titleFont);
    root->addWidget(title);

    m_codeLabel = new QLabel(info.code.isEmpty() ? QStringLiteral("错误码：未知")
                                                  : QStringLiteral("错误码：%1").arg(info.code),
                             this);
    m_codeLabel->setObjectName(QStringLiteral("PlaybackErrorCode"));
    root->addWidget(m_codeLabel);

    m_detailLabel = new QLabel(info.detail.isEmpty() ? QStringLiteral("播放请求失败，请稍后重试。")
                                                     : info.detail,
                               this);
    m_detailLabel->setObjectName(QStringLiteral("PlaybackErrorDetail"));
    m_detailLabel->setWordWrap(true);
    m_detailLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_detailLabel);
    root->addStretch(1);

    auto *buttons = new QHBoxLayout();
    buttons->setSpacing(8);
    m_exportButton = new QPushButton(QStringLiteral("导出诊断"), this);
    m_exportButton->setObjectName(QStringLiteral("PlaybackErrorExportButton"));
    m_exportButton->setProperty("role", QStringLiteral("ghost"));
    m_exportButton->setEnabled(!info.diagnostics.isEmpty());
    buttons->addWidget(m_exportButton);
    buttons->addStretch(1);

    m_backButton = new QPushButton(QStringLiteral("返回"), this);
    m_backButton->setObjectName(QStringLiteral("PlaybackErrorBackButton"));
    m_backButton->setProperty("role", QStringLiteral("secondary"));
    buttons->addWidget(m_backButton);

    m_retryButton = new QPushButton(QStringLiteral("重试"), this);
    m_retryButton->setObjectName(QStringLiteral("PlaybackErrorRetryButton"));
    m_retryButton->setProperty("role", QStringLiteral("primary"));
    m_retryButton->setDefault(true);
    m_retryButton->setEnabled(info.retryEnabled);
    buttons->addWidget(m_retryButton);
    root->addLayout(buttons);

    connect(m_retryButton, &QPushButton::clicked, this, [this]() {
        m_action = Action::Retry;
        accept();
    });
    connect(m_backButton, &QPushButton::clicked, this, [this]() {
        m_action = Action::Back;
        reject();
    });
    connect(m_exportButton, &QPushButton::clicked, this, [this]() {
        m_action = Action::Export;
        exportDiagnostics();
    });
}

void PlaybackErrorDialog::exportDiagnostics()
{
    if (m_info.diagnostics.isEmpty())
        return;
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出播放诊断"), QStringLiteral("playback-error.txt"),
        QStringLiteral("Text files (*.txt)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    QTextStream out(&file);
    out << QStringLiteral("code: %1\n").arg(m_info.code);
    out << QStringLiteral("detail: %1\n\n").arg(m_info.detail);
    out << m_info.diagnostics;
}

PlaybackErrorDialog::Action PlaybackErrorDialog::show(QWidget *parent, const Info &info)
{
    PlaybackErrorDialog dialog(info, parent);
    dialog.exec();
    return dialog.action();
}

} // namespace ui
