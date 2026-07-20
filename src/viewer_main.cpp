#include "bookmark_store.h"
#include "archive_manifest.h"
#include "archive_tool_locator.h"

#include <QAction>
#include <QApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QColor>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontDialog>
#include <QHash>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QLineEdit>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>
#include <QSplitter>
#include <QStatusBar>
#include <QShortcut>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QToolBar>
#include <QToolButton>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoWidget>

namespace {

struct Fragment {
    qint64 offset = 0;
    QString text;
};

struct Article {
    QString id;
    QString title;
    QString date;
    QString section;
    QString category;
    QString keywords;
    int firstFragment = 0;
    int afterLastFragment = 0;
};

struct Book {
    QString name;
    QVector<Fragment> fragments;
    QVector<Article> articles;
    QVector<Article> catalogArticles;
    QStringList monthIndexes;
    QStringList media;
    QStringList availableMedia;
    QStringList missingMedia;
};

QString articleKey(const QString &book, const QString &id)
{
    return book.toUpper() + QLatin1Char('/') + id;
}

QString monthLabel(const QString &id)
{
    return QStringLiteral("19") + id.mid(1, 2) + QStringLiteral(".") + id.mid(3, 2);
}

QString issueMonthLabel(const QString &date)
{
    static const QRegularExpression monthExpression(QStringLiteral("^(\\d{4}\\.\\d{2})"));
    const QRegularExpressionMatch match = monthExpression.match(date);
    return match.hasMatch() ? match.captured(1) : QStringLiteral("Unknown month");
}

QString escapedParagraph(const QString &text)
{
    return text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>\n"));
}

bool isHeadingLike(const QString &text)
{
    const QString value = text.simplified();
    if (value.size() < 2 || value.size() > 100 || value.contains(QStringLiteral("MVBMP2"))
        || value.contains(QLatin1Char('`')) || value.contains(QRegularExpression(QStringLiteral("[.!?]"))))
        return false;
    int KoreanCharacters = 0;
    for (const QChar character : value) {
        if (character.unicode() >= 0xac00 && character.unicode() <= 0xd7a3)
            ++KoreanCharacters;
    }
    return KoreanCharacters >= 2 || value.count(QRegularExpression(QStringLiteral("[A-Za-z]"))) >= 6;
}

class IsoUnpackDialog final : public QDialog {
public:
    IsoUnpackDialog(QString builderPath, QString isoPath, QString outputDirectory,
                    bool rebuild, QWidget *parent = nullptr)
        : QDialog(parent)
        , builderPath_(std::move(builderPath))
        , isoPath_(std::move(isoPath))
        , outputDirectory_(QDir(outputDirectory).absolutePath())
        , rebuild_(rebuild)
    {
        setWindowTitle(QStringLiteral("ISO unpack"));
        setModal(true);
        resize(720, 420);
        auto *layout = new QVBoxLayout(this);
        status_ = new QLabel(this);
        status_->setWordWrap(true);
        layout->addWidget(status_);
        log_ = new QPlainTextEdit(this);
        log_->setReadOnly(true);
        log_->setLineWrapMode(QPlainTextEdit::NoWrap);
        layout->addWidget(log_, 1);
        cancelButton_ = new QPushButton(QStringLiteral("Cancel"), this);
        layout->addWidget(cancelButton_, 0, Qt::AlignRight);
        process_ = new QProcess(this);
        process_->setProcessChannelMode(QProcess::SeparateChannels);
        connect(cancelButton_, &QPushButton::clicked, this, [this] { cancel(); });
        connect(process_, &QProcess::readyReadStandardOutput, this, [this] {
            appendOutput(process_->readAllStandardOutput());
        });
        connect(process_, &QProcess::readyReadStandardError, this, [this] {
            appendOutput(process_->readAllStandardError());
        });
        connect(process_, QOverload<QProcess::ProcessError>::of(&QProcess::errorOccurred), this,
                [this](QProcess::ProcessError error) {
                    if (error == QProcess::FailedToStart) {
                        lastProcessError_ = process_->errorString();
                        if (!terminal_)
                            finishFailure(QStringLiteral("Cannot start archive builder: %1")
                                              .arg(lastProcessError_));
                    }
                });
        connect(process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this](int exitCode, QProcess::ExitStatus exitStatus) { processFinished(exitCode, exitStatus); });
    }

    void start()
    {
        startProcess(Stage::BuilderVersion, {QStringLiteral("--version")},
                     QStringLiteral("Checking the packaged archive builder..."));
    }

    bool succeeded() const { return succeeded_; }

protected:
    void reject() override
    {
        if (process_->state() != QProcess::NotRunning) {
            cancel();
            return;
        }
        QDialog::reject();
    }

private:
    enum class Stage { BuilderVersion, ToolCheck, Unpack };

    void appendOutput(const QByteArray &bytes)
    {
        const QString output = QString::fromLocal8Bit(bytes).trimmed();
        if (!output.isEmpty())
            log_->appendPlainText(output);
    }

    void startProcess(Stage stage, const QStringList &arguments, const QString &status)
    {
        stage_ = stage;
        lastProcessError_.clear();
        status_->setText(status);
        log_->appendPlainText(QStringLiteral("$ %1 %2").arg(builderPath_, arguments.join(QLatin1Char(' '))));
        process_->setProgram(builderPath_);
        process_->setArguments(arguments);
        process_->start();
    }

    void processFinished(int exitCode, QProcess::ExitStatus exitStatus)
    {
        appendOutput(process_->readAllStandardOutput());
        appendOutput(process_->readAllStandardError());
        if (terminal_)
            return;
        if (cancelRequested_) {
            finishFailure(QStringLiteral("ISO unpack was canceled. Any partial output was left in place: %1")
                              .arg(outputDirectory_));
            return;
        }
        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
            QString detail = lastProcessError_;
            if (detail.isEmpty())
                detail = QStringLiteral("archive builder exited with code %1.").arg(exitCode);
            finishFailure(QStringLiteral("%1\nSee the log below for details.").arg(detail));
            return;
        }
        switch (stage_) {
        case Stage::BuilderVersion:
            startProcess(Stage::ToolCheck, {QStringLiteral("--check-tools")},
                         QStringLiteral("Checking the built-in ISO extractor..."));
            return;
        case Stage::ToolCheck: {
            QStringList arguments;
            if (rebuild_)
                arguments.append(QStringLiteral("--rebuild"));
            arguments.append(isoPath_);
            arguments.append(outputDirectory_);
            startProcess(Stage::Unpack, arguments,
                         QStringLiteral("Unpacking and converting the ISO. This can take a while..."));
            return;
        }
        case Stage::Unpack:
            break;
        }
        if (!QFileInfo(QDir(outputDirectory_).filePath(QStringLiteral("manifest.json"))).isFile()) {
            finishFailure(QStringLiteral("The archive builder finished without producing manifest.json."));
            return;
        }
        succeeded_ = true;
        terminal_ = true;
        status_->setText(QStringLiteral("ISO unpack completed. Opening the converted archive..."));
        accept();
    }

    void cancel()
    {
        if (process_->state() == QProcess::NotRunning) {
            reject();
            return;
        }
        if (cancelRequested_)
            return;
        cancelRequested_ = true;
        status_->setText(QStringLiteral("Canceling archive builder..."));
        cancelButton_->setEnabled(false);
        process_->terminate();
        QTimer::singleShot(3000, this, [this] {
            if (process_->state() != QProcess::NotRunning)
                process_->kill();
        });
    }

    void finishFailure(const QString &message)
    {
        terminal_ = true;
        status_->setText(message);
        cancelButton_->setText(QStringLiteral("Close"));
        cancelButton_->setEnabled(true);
    }

    QString builderPath_;
    QString isoPath_;
    QString outputDirectory_;
    bool rebuild_ = false;
    bool cancelRequested_ = false;
    bool terminal_ = false;
    bool succeeded_ = false;
    Stage stage_ = Stage::BuilderVersion;
    QString lastProcessError_;
    QLabel *status_ = nullptr;
    QPlainTextEdit *log_ = nullptr;
    QPushButton *cancelButton_ = nullptr;
    QProcess *process_ = nullptr;
};

class Viewer final : public QMainWindow {
public:
    explicit Viewer(const QString &initialDirectory = {}, QWidget *parent = nullptr)
        : QMainWindow(parent)
    {
        setWindowTitle(QStringLiteral("MYCOM Viewer"));
        resize(1280, 820);

        auto *splitter = new QSplitter(this);
        tree_ = new QTreeWidget(splitter);
        tree_->setHeaderLabel(QStringLiteral("MYCOM archive"));
        tree_->setMinimumWidth(300);
        browser_ = new QTextBrowser(splitter);
        browser_->setOpenLinks(false);
        defaultFont_ = browser_->font();
        zoomInButton_ = new QToolButton(browser_->viewport());
        zoomOutButton_ = new QToolButton(browser_->viewport());
        for (QToolButton *button : {zoomInButton_, zoomOutButton_}) {
            button->setFixedSize(42, 36);
            button->setAutoRaise(false);
            button->setStyleSheet(QStringLiteral("QToolButton{background:rgba(30,41,59,150);color:white;"
                                                  "border:1px solid rgba(255,255,255,120);border-radius:8px;"
                                                  "font-weight:700;}QToolButton:hover{background:rgba(30,41,59,215);}"));
        }
        zoomInButton_->setText(QStringLiteral("A+"));
        zoomInButton_->setToolTip(QStringLiteral("Increase content text by 10%"));
        zoomOutButton_->setText(QStringLiteral("A−"));
        zoomOutButton_->setToolTip(QStringLiteral("Decrease content text by 10%"));
        contentFindBox_ = new QLineEdit(browser_->viewport());
        contentFindBox_->setPlaceholderText(QStringLiteral("Find in this content"));
        contentFindBox_->setClearButtonEnabled(true);
        contentFindBox_->setToolTip(QStringLiteral("Find in this article only (case-insensitive)"));
        contentFindCloseButton_ = new QToolButton(browser_->viewport());
        contentFindCloseButton_->setText(QStringLiteral("×"));
        contentFindCloseButton_->setToolTip(QStringLiteral("Close content find"));
        contentFindCloseButton_->setStyleSheet(QStringLiteral("QToolButton{background:rgba(30,41,59,150);color:white;"
                                                               "border:1px solid rgba(255,255,255,120);border-radius:8px;"
                                                               "font-weight:700;}QToolButton:hover{background:rgba(30,41,59,215);}"));
        contentFindBox_->hide();
        contentFindCloseButton_->hide();
        browser_->viewport()->installEventFilter(this);
        connect(zoomInButton_, &QToolButton::clicked, this, [this] { changeContentZoom(10); });
        connect(zoomOutButton_, &QToolButton::clicked, this, [this] { changeContentZoom(-10); });
        connect(contentFindBox_, &QLineEdit::textChanged, this, [this] { findInContent(false); });
        connect(contentFindBox_, &QLineEdit::returnPressed, this, [this] { findInContent(true); });
        connect(contentFindCloseButton_, &QToolButton::clicked, this, [this] { hideContentFind(); });
        auto *contentFindShortcut = new QShortcut(QKeySequence::Find, browser_);
        contentFindShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(contentFindShortcut, &QShortcut::activated, this, [this] { showContentFind(); });
        auto *contentFindEscapeShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), contentFindBox_);
        contentFindEscapeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(contentFindEscapeShortcut, &QShortcut::activated, this, [this] { hideContentFind(); });
        splitter->addWidget(tree_);
        splitter->addWidget(browser_);
        splitter->setStretchFactor(1, 1);
        setCentralWidget(splitter);
        positionZoomButtons();

        connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) { openItem(item); });
        connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) { openItem(item); });
        tree_->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(tree_, &QTreeWidget::customContextMenuRequested, this,
                [this](const QPoint &position) { showBookmarkContextMenu(position); });
        connect(browser_, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) { openLink(url); });

        auto *fileMenu = menuBar()->addMenu(QStringLiteral("File"));
        auto *openAction = fileMenu->addAction(QStringLiteral("Open converted archive..."));
        openAction->setShortcut(QKeySequence::Open);
        connect(openAction, &QAction::triggered, this, [this] { chooseDirectory(); });
        auto *unpackAction = fileMenu->addAction(QStringLiteral("ISO unpack..."));
        connect(unpackAction, &QAction::triggered, this, [this] { chooseIsoUnpack(); });
        fileMenu->addSeparator();
        auto *quitAction = fileMenu->addAction(QStringLiteral("Quit"));
        quitAction->setShortcut(QKeySequence::Quit);
        connect(quitAction, &QAction::triggered, this, &QWidget::close);

        auto *viewMenu = menuBar()->addMenu(QStringLiteral("View"));
        auto *searchAction = viewMenu->addAction(QStringLiteral("Search archive"));
        searchAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")));
        connect(searchAction, &QAction::triggered, this, [this] { performSearch(); });
        auto *clearFilterAction = viewMenu->addAction(QStringLiteral("Clear filter"));
        connect(clearFilterAction, &QAction::triggered, this, [this] {
            searchBox_->clear();
            applyTreeFilter();
        });
        viewMenu->addSeparator();
        auto *fontAction = viewMenu->addAction(QStringLiteral("Select content font..."));
        connect(fontAction, &QAction::triggered, this, [this] { chooseFont(); });
        auto *resetFontAction = viewMenu->addAction(QStringLiteral("Restore default content font"));
        connect(resetFontAction, &QAction::triggered, this, [this] { restoreDefaultFont(); });

        auto *bookmarkMenu = menuBar()->addMenu(QStringLiteral("Bookmarks"));
        addBookmarkAction_ = bookmarkMenu->addAction(QStringLiteral("Add current page"));
        addBookmarkAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+D")));
        connect(addBookmarkAction_, &QAction::triggered, this, [this] { addCurrentBookmark(); });
        removeBookmarkAction_ = bookmarkMenu->addAction(QStringLiteral("Remove current page"));
        connect(removeBookmarkAction_, &QAction::triggered, this, [this] { removeCurrentBookmark(); });
        clearBookmarksAction_ = bookmarkMenu->addAction(QStringLiteral("Clear all bookmarks..."));
        connect(clearBookmarksAction_, &QAction::triggered, this, [this] { clearBookmarks(); });
        bookmarkMenu->addSeparator();
        showBookmarksAction_ = bookmarkMenu->addAction(QStringLiteral("Show bookmarks"));
        connect(showBookmarksAction_, &QAction::triggered, this, [this] { showBookmarks(); });

        auto *helpMenu = menuBar()->addMenu(QStringLiteral("Help"));
        auto *aboutAction = helpMenu->addAction(QStringLiteral("About MYCOM Viewer"));
        connect(aboutAction, &QAction::triggered, this, [this] { showAbout(); });

        auto *toolbar = addToolBar(QStringLiteral("Search"));
        searchBox_ = new QLineEdit(toolbar);
        searchBox_->setPlaceholderText(QStringLiteral("Search titles and recovered text"));
        searchBox_->setClearButtonEnabled(true);
        toolbar->addWidget(searchBox_);
        auto *toolbarSearch = toolbar->addAction(QStringLiteral("Search"));
        connect(toolbarSearch, &QAction::triggered, this, [this] { performSearch(); });
        connect(searchBox_, &QLineEdit::returnPressed, this, [this] { performSearch(); });
        connect(searchBox_, &QLineEdit::textChanged, this, [this] { applyTreeFilter(); });

        loadFontSettings();

        browser_->setHtml(QStringLiteral("<h1>MYCOM Viewer</h1>"
                                         "<p>Open an archive directory produced by <code>mycom-archive-build</code>.</p>"));
        player_ = new QMediaPlayer(this);
        updateBookmarkActions();
        if (!initialDirectory.isEmpty())
            loadDirectory(initialDirectory);
    }

private:
    QTreeWidget *tree_ = nullptr;
    QTextBrowser *browser_ = nullptr;
    QString rootDirectory_;
    QString assetDirectory_;
    QVector<Book> books_;
    QHash<QString, QPair<int, int>> articleLocations_;
    QHash<QString, QString> crossBookArticleLocations_;
    QHash<QString, QPair<int, int>> catalogLocations_;
    QHash<QString, int> bookLocations_;
    QHash<QString, QString> mediaFiles_;
    QMediaPlayer *player_ = nullptr;
    QLineEdit *searchBox_ = nullptr;
    QLineEdit *contentFindBox_ = nullptr;
    QToolButton *zoomInButton_ = nullptr;
    QToolButton *zoomOutButton_ = nullptr;
    QToolButton *contentFindCloseButton_ = nullptr;
    QAction *addBookmarkAction_ = nullptr;
    QAction *removeBookmarkAction_ = nullptr;
    QAction *clearBookmarksAction_ = nullptr;
    QAction *showBookmarksAction_ = nullptr;
    QString currentPageKey_;
    QString archiveSignature_;
    QStringList bookmarks_;
    QFont defaultFont_;
    QFont viewerFont_;
    int contentZoomPercent_ = 100;

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (browser_ && watched == browser_->viewport()
            && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
            positionZoomButtons();
        }
        return QMainWindow::eventFilter(watched, event);
    }

    void positionZoomButtons()
    {
        if (!browser_ || !zoomInButton_ || !zoomOutButton_ || !contentFindBox_ || !contentFindCloseButton_)
            return;
        QWidget *viewport = browser_->viewport();
        const int right = qMax(8, viewport->width() - zoomInButton_->width() - 14);
        zoomInButton_->move(right, 14);
        zoomOutButton_->move(right, 56);
        const int findWidth = qMax(120, qMin(280, viewport->width() - zoomInButton_->width() - 40));
        contentFindBox_->setGeometry(12, 14, findWidth, 36);
        contentFindCloseButton_->setGeometry(16 + findWidth, 14, 36, 36);
        zoomInButton_->raise();
        zoomOutButton_->raise();
        contentFindBox_->raise();
        contentFindCloseButton_->raise();
    }

    void showContentFind()
    {
        if (!browser_ || !contentFindBox_)
            return;
        contentFindBox_->show();
        contentFindCloseButton_->show();
        contentFindBox_->setFocus();
        contentFindBox_->selectAll();
        positionZoomButtons();
    }

    void hideContentFind()
    {
        if (!contentFindBox_)
            return;
        contentFindBox_->hide();
        contentFindCloseButton_->hide();
        browser_->setExtraSelections({});
        browser_->setFocus();
    }

    void updateContentHighlights()
    {
        QList<QTextEdit::ExtraSelection> highlights;
        if (!contentFindBox_ || contentFindBox_->text().isEmpty()) {
            browser_->setExtraSelections(highlights);
            return;
        }
        QTextCursor cursor(browser_->document());
        while (true) {
            cursor = browser_->document()->find(contentFindBox_->text(), cursor, QTextDocument::FindFlags());
            if (cursor.isNull())
                break;
            QTextEdit::ExtraSelection selection;
            selection.cursor = cursor;
            selection.format.setBackground(QColor(QStringLiteral("#fff59d")));
            selection.format.setForeground(QColor(QStringLiteral("#1f2937")));
            highlights.append(selection);
        }
        browser_->setExtraSelections(highlights);
    }

    void findInContent(bool nextMatch)
    {
        if (!contentFindBox_ || contentFindBox_->text().isEmpty())
            return;
        updateContentHighlights();
        const QString text = contentFindBox_->text();
        if (!nextMatch) {
            QTextCursor start = browser_->textCursor();
            start.movePosition(QTextCursor::Start);
            browser_->setTextCursor(start);
        }
        if (browser_->find(text, QTextDocument::FindFlags()))
            return;
        QTextCursor start = browser_->textCursor();
        start.movePosition(QTextCursor::Start);
        browser_->setTextCursor(start);
        if (!browser_->find(text, QTextDocument::FindFlags()))
            statusBar()->showMessage(QStringLiteral("No match in this content."), 2500);
    }

    void refreshCurrentPage()
    {
        if (currentPageKey_.startsWith(QStringLiteral("article:")))
            showArticle(currentPageKey_.mid(8));
        else if (currentPageKey_.startsWith(QStringLiteral("booktext:")))
            showRecoveredBook(currentPageKey_.mid(9));
    }

    void changeContentZoom(int delta)
    {
        if (!currentPageKey_.startsWith(QStringLiteral("article:"))
            && !currentPageKey_.startsWith(QStringLiteral("booktext:"))) {
            statusBar()->showMessage(QStringLiteral("Open an article or recovered text to change content size."), 3000);
            return;
        }
        const int next = qBound(50, contentZoomPercent_ + delta, 300);
        if (next == contentZoomPercent_)
            return;
        contentZoomPercent_ = next;
        refreshCurrentPage();
        statusBar()->showMessage(QStringLiteral("Content text size: %1%").arg(contentZoomPercent_), 3000);
    }

    QString settingsFilePath() const
    {
        QString directory = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        if (directory.isEmpty())
            directory = QDir::homePath();
        QDir().mkpath(directory);
        return QDir(directory).filePath(QStringLiteral("mycom-viewer.ini"));
    }

    void applyViewerFont(const QFont &font, bool refreshPage)
    {
        viewerFont_ = font;
        browser_->setFont(viewerFont_);
        browser_->document()->setDefaultFont(viewerFont_);
        if (refreshPage && !currentPageKey_.isEmpty())
            refreshCurrentPage();
    }

    void loadFontSettings()
    {
        QSettings settings(settingsFilePath(), QSettings::IniFormat);
        QFont saved = defaultFont_;
        if (settings.contains(QStringLiteral("font/serialized"))
            && saved.fromString(settings.value(QStringLiteral("font/serialized")).toString())) {
            applyViewerFont(saved, false);
            return;
        }
        applyViewerFont(defaultFont_, false);
    }

    void chooseFont()
    {
        bool accepted = false;
        const QFont selected = QFontDialog::getFont(&accepted, viewerFont_, this,
                                                    QStringLiteral("Select MYCOM Viewer font"));
        if (!accepted)
            return;
        applyViewerFont(selected, true);
        QSettings settings(settingsFilePath(), QSettings::IniFormat);
        settings.setValue(QStringLiteral("font/serialized"), selected.toString());
        settings.sync();
        statusBar()->showMessage(QStringLiteral("Font saved to ") + settings.fileName(), 6000);
    }

    void restoreDefaultFont()
    {
        applyViewerFont(defaultFont_, true);
        QSettings settings(settingsFilePath(), QSettings::IniFormat);
        settings.remove(QStringLiteral("font/serialized"));
        settings.sync();
        statusBar()->showMessage(QStringLiteral("Default font restored; setting removed from ")
                                 + settings.fileName(), 6000);
    }

    QString contentStyle() const
    {
        QString family = viewerFont_.family();
        family.replace(QLatin1Char('\''), QStringLiteral("\\\\'"));
        const qreal pointSize = (viewerFont_.pointSizeF() > 0 ? viewerFont_.pointSizeF() : 12.0)
                                * contentZoomPercent_ / 100.0;
        return QStringLiteral("<style>body{font-family:'%1';line-height:1.7;margin:1.5em;max-width:72em}"
                              "h1{font-size:%3pt}.article-text{font-size:%2pt;margin:.75em 0}"
                              ".article-heading{font-size:%4pt;margin:1.5em 0 .45em}"
                              ".image-line{margin:1.4em 0 .35em;text-align:center}.image-line img{max-width:100%;height:auto}"
                              ".source-label{margin:.2em 0 1.4em;color:#667085;font-family:monospace;font-size:%5pt;text-align:center;word-break:break-all}"
                              ".figure-caption{margin:.2em 0 1.4em;text-align:center;color:#4b5563;font-size:%6pt}</style>")
            .arg(family, QString::number(pointSize, 'f', 1), QString::number(pointSize * 1.75, 'f', 1),
                 QString::number(pointSize * 1.25, 'f', 1), QString::number(pointSize * .8, 'f', 1),
                 QString::number(pointSize * .95, 'f', 1));
    }

    bool isDecorativeImageReference(const QString &reference) const
    {
        const QString baseName = QFileInfo(reference).completeBaseName().toLower();
        return baseName.startsWith(QStringLiteral("ico")) || baseName.startsWith(QStringLiteral("jicon"));
    }

    QString imageHtml(const QString &reference, const QString &image, const QString &link) const
    {
        QString html = QStringLiteral("<p class=\"image-line\">");
        if (!link.isEmpty())
            html += QStringLiteral("<a href=\"") + link.toHtmlEscaped() + QStringLiteral("\">");
        html += QStringLiteral("<img src=\"") + image.toHtmlEscaped() + QStringLiteral("\" alt=\"")
                + reference.toHtmlEscaped() + QStringLiteral("\">");
        if (!link.isEmpty())
            html += QStringLiteral("</a>");
        html += QStringLiteral("</p><p class=\"source-label\">") + reference.toHtmlEscaped()
                + QStringLiteral("</p>");
        return html;
    }

    void chooseDirectory()
    {
        const QString directory = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Open MYCOM archive"), rootDirectory_);
        if (!directory.isEmpty())
            loadDirectory(directory);
    }

    void chooseIsoUnpack()
    {
        QSettings settings(settingsFilePath(), QSettings::IniFormat);
        const QString lastIso = settings.value(QStringLiteral("isoUnpack/lastIsoPath")).toString();
        const QString isoPath = QFileDialog::getOpenFileName(
            this, QStringLiteral("Select MYCOM ISO"),
            QFileInfo(lastIso).isFile() ? QFileInfo(lastIso).absolutePath() : QDir::homePath(),
            QStringLiteral("ISO images (*.iso *.ISO);;All files (*)"));
        if (isoPath.isEmpty())
            return;
        const QFileInfo isoInfo(isoPath);
        if (!isoInfo.isFile()) {
            QMessageBox::warning(this, QStringLiteral("ISO unpack"),
                                 QStringLiteral("The selected ISO file is not readable."));
            return;
        }

        const QString builderPath = ArchiveToolLocator::findArchiveBuilder(QCoreApplication::applicationDirPath());
        if (builderPath.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("ISO unpack"),
                                 QStringLiteral("The packaged mycom-archive-build command-line tool was not found or is not executable.\n\n"
                                                "Expected locations:\n%1")
                                     .arg(ArchiveToolLocator::archiveBuilderCandidates(
                                         QCoreApplication::applicationDirPath()).join(QLatin1Char('\n'))));
            return;
        }

        const QString lastOutput = settings.value(QStringLiteral("isoUnpack/lastOutputDirectory")).toString();
        const QString outputDirectory = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Select empty archive output directory"),
            QFileInfo(lastOutput).isDir() ? lastOutput : isoInfo.absolutePath());
        if (outputDirectory.isEmpty())
            return;
        QDir output(outputDirectory);
        const bool nonEmptyOutput = output.exists()
            && !output.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty();
        bool rebuild = false;
        if (nonEmptyOutput) {
            if (!QFileInfo(output.filePath(QStringLiteral("manifest.json"))).isFile()) {
                QMessageBox::warning(this, QStringLiteral("ISO unpack"),
                                     QStringLiteral("The output directory is not empty. Choose an empty directory.\n%1")
                                         .arg(output.absolutePath()));
                return;
            }
            if (QMessageBox::question(
                    this, QStringLiteral("Rebuild existing archive"),
                    QStringLiteral("This directory contains an existing MYCOM archive. Rebuilding it will delete and replace its contents.\n\n%1")
                        .arg(output.absolutePath()),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
                return;
            }
            rebuild = true;
        }

        settings.setValue(QStringLiteral("isoUnpack/lastIsoPath"), isoInfo.absoluteFilePath());
        settings.setValue(QStringLiteral("isoUnpack/lastOutputDirectory"), output.absolutePath());
        settings.sync();

        IsoUnpackDialog progress(builderPath, isoInfo.absoluteFilePath(), output.absolutePath(), rebuild, this);
        progress.start();
        progress.exec();
        if (progress.succeeded())
            loadDirectory(output.absolutePath());
    }

    void showAbout()
    {
        QMessageBox::about(this, QStringLiteral("About MYCOM Viewer"),
                           QStringLiteral("<h2>MYCOM Viewer</h2>"
                                          "<p>A Qt5 multi-platform viewer for the recovered MYCOM "
                                          "Microsoft Multimedia Viewer archive.</p>"
                                          "<p>It opens archives created by <code>mycom-archive-build</code> "
                                          "and provides article browsing, search, bookmarks, and legacy media playback.</p>"
                                          "<p>Version %1 &middot; Qt %2</p>")
                               .arg(QCoreApplication::applicationVersion().toHtmlEscaped(),
                                    QString::fromLatin1(qVersion()).toHtmlEscaped()));
    }

    QString pageLabel(const QString &key) const
    {
        if (key.startsWith(QStringLiteral("article:"))) {
            const QString article = key.mid(8);
            const auto location = articleLocations_.constFind(article);
            if (location != articleLocations_.constEnd()) {
                const Article &entry = books_.at(location->first).articles.at(location->second);
                return books_.at(location->first).name + QStringLiteral(" / ") + entry.title;
            }
        }
        if (key.startsWith(QStringLiteral("booktext:")))
            return key.mid(9) + QStringLiteral(" / Recovered text");
        if (key.startsWith(QStringLiteral("catalog:"))) {
            const auto location = catalogLocations_.constFind(key.mid(8));
            if (location != catalogLocations_.constEnd()) {
                const Article &entry = books_.at(location->first).catalogArticles.at(location->second);
                return books_.at(location->first).name + QStringLiteral(" / ") + entry.title;
            }
        }
        if (key.startsWith(QStringLiteral("month:"))) {
            const QStringList parts = key.mid(6).split(QLatin1Char('/'));
            if (parts.size() == 2)
                return parts.at(0) + QStringLiteral(" / ") + monthLabel(parts.at(1));
        }
        return key;
    }

    BookmarkStore bookmarkStore() const
    {
        return BookmarkStore(settingsFilePath(), archiveSignature_);
    }

    bool isBookmarkTargetAvailable(const QString &target) const
    {
        if (target.startsWith(QStringLiteral("article:")))
            return articleLocations_.contains(target.mid(8));
        if (target.startsWith(QStringLiteral("booktext:")))
            return bookLocations_.contains(target.mid(9));
        return false;
    }

    bool isBookmarkTreeItem(const QTreeWidgetItem *item) const
    {
        return item && item->parent()
            && item->parent()->data(0, Qt::UserRole).toString() == QStringLiteral("bookmarks");
    }

    QString bookmarkTargetForItem(const QTreeWidgetItem *item) const
    {
        return isBookmarkTreeItem(item) ? item->data(0, Qt::UserRole + 1).toString() : QString();
    }

    void updateBookmarkActions()
    {
        const bool canBookmark = isBookmarkTargetAvailable(currentPageKey_);
        if (addBookmarkAction_)
            addBookmarkAction_->setEnabled(canBookmark && !bookmarks_.contains(currentPageKey_));
        if (removeBookmarkAction_)
            removeBookmarkAction_->setEnabled(canBookmark && bookmarks_.contains(currentPageKey_));
        if (clearBookmarksAction_)
            clearBookmarksAction_->setEnabled(!bookmarks_.isEmpty());
        if (showBookmarksAction_)
            showBookmarksAction_->setEnabled(!bookmarks_.isEmpty());
    }

    bool saveBookmarks()
    {
        QString error;
        if (archiveSignature_.isEmpty() || !bookmarkStore().save(bookmarks_, &error)) {
            QMessageBox::warning(this, QStringLiteral("MYCOM Viewer"),
                                 error.isEmpty() ? QStringLiteral("No converted archive is open.") : error);
            return false;
        }
        return true;
    }

    void migrateLegacyBookmarksIfNeeded()
    {
        if (archiveSignature_.isEmpty())
            return;
        BookmarkStore store = bookmarkStore();
        if (store.hasStoredValue())
            return;

        QSettings legacySettings;
        const QStringList legacy = legacySettings.value(QStringLiteral("mycomViewer/bookmarks")).toStringList();
        QStringList migrated;
        for (const QString &target : legacy) {
            if (isBookmarkTargetAvailable(target) && !migrated.contains(target))
                migrated.append(target);
        }
        QString error;
        if (store.save(migrated, &error)) {
            legacySettings.remove(QStringLiteral("mycomViewer/bookmarks"));
            legacySettings.sync();
        } else if (!error.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("Legacy bookmarks were not migrated: ") + error, 6000);
        }
    }

    void loadBookmarks()
    {
        bookmarks_.clear();
        if (archiveSignature_.isEmpty()) {
            updateBookmarkActions();
            return;
        }
        migrateLegacyBookmarksIfNeeded();
        QString error;
        const QStringList loaded = bookmarkStore().load(&error);
        if (!error.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("MYCOM Viewer"), error);
        } else {
            for (const QString &target : loaded) {
                if (!target.isEmpty() && !bookmarks_.contains(target))
                    bookmarks_.append(target);
            }
        }
        updateBookmarkActions();
    }

    void rebuildBookmarks()
    {
        QList<QTreeWidgetItem *> obsolete;
        for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
            QTreeWidgetItem *item = tree_->topLevelItem(index);
            if (item->data(0, Qt::UserRole).toString() == QStringLiteral("bookmarks"))
                obsolete.append(item);
        }
        for (QTreeWidgetItem *item : obsolete)
            delete tree_->takeTopLevelItem(tree_->indexOfTopLevelItem(item));

        if (bookmarks_.isEmpty()) {
            updateBookmarkActions();
            return;
        }
        auto *root = new QTreeWidgetItem(tree_, {QStringLiteral("Bookmarks (%1)").arg(bookmarks_.size())});
        root->setData(0, Qt::UserRole, QStringLiteral("bookmarks"));
        for (const QString &target : bookmarks_) {
            const bool available = isBookmarkTargetAvailable(target);
            auto *item = new QTreeWidgetItem(root, {available ? pageLabel(target)
                                                               : QStringLiteral("Unavailable: ") + target});
            item->setData(0, Qt::UserRole, available ? target : QStringLiteral("bookmark-unavailable"));
            item->setData(0, Qt::UserRole + 1, target);
            item->setToolTip(0, available ? target : QStringLiteral("This page is not present in the open archive."));
            item->setDisabled(!available);
        }
        root->setExpanded(true);
        updateBookmarkActions();
    }

    void addCurrentBookmark()
    {
        if (!isBookmarkTargetAvailable(currentPageKey_)) {
            statusBar()->showMessage(QStringLiteral("Open an article or recovered text before bookmarking it."), 4000);
            return;
        }
        if (bookmarks_.contains(currentPageKey_)) {
            statusBar()->showMessage(QStringLiteral("This page is already bookmarked."), 3000);
            return;
        }
        bookmarks_.append(currentPageKey_);
        if (!saveBookmarks()) {
            bookmarks_.removeLast();
            return;
        }
        rebuildBookmarks();
        statusBar()->showMessage(QStringLiteral("Bookmark saved: ") + pageLabel(currentPageKey_), 4000);
    }

    void removeBookmarkTarget(const QString &target)
    {
        const int index = bookmarks_.indexOf(target);
        if (index < 0)
            return;
        bookmarks_.removeAt(index);
        if (!saveBookmarks()) {
            bookmarks_.insert(index, target);
            return;
        }
        rebuildBookmarks();
        statusBar()->showMessage(QStringLiteral("Bookmark removed."), 3000);
    }

    void removeCurrentBookmark()
    {
        removeBookmarkTarget(currentPageKey_);
    }

    void clearBookmarks()
    {
        if (bookmarks_.isEmpty())
            return;
        if (QMessageBox::question(this, QStringLiteral("Clear all bookmarks"),
                                  QStringLiteral("Remove all bookmarks for this converted archive?"))
            != QMessageBox::Yes) {
            return;
        }
        const QStringList previous = bookmarks_;
        bookmarks_.clear();
        if (!saveBookmarks()) {
            bookmarks_ = previous;
            return;
        }
        rebuildBookmarks();
        statusBar()->showMessage(QStringLiteral("All bookmarks were removed."), 3000);
    }

    void showBookmarks()
    {
        rebuildBookmarks();
        for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
            QTreeWidgetItem *item = tree_->topLevelItem(index);
            if (item->data(0, Qt::UserRole).toString() == QStringLiteral("bookmarks")) {
                tree_->setCurrentItem(item);
                tree_->scrollToItem(item);
                return;
            }
        }
    }

    void showBookmarkContextMenu(const QPoint &position)
    {
        QTreeWidgetItem *item = tree_->itemAt(position);
        const QString target = bookmarkTargetForItem(item);
        if (target.isEmpty())
            return;
        QMenu menu(tree_);
        QAction *removeAction = menu.addAction(QStringLiteral("Remove bookmark"));
        if (menu.exec(tree_->viewport()->mapToGlobal(position)) == removeAction)
            removeBookmarkTarget(target);
    }

    bool filterItem(QTreeWidgetItem *item, const QString &query)
    {
        bool matches = query.isEmpty() || item->text(0).contains(query, Qt::CaseInsensitive);
        for (int index = 0; index < item->childCount(); ++index)
            matches = filterItem(item->child(index), query) || matches;
        item->setHidden(!matches);
        return matches;
    }

    void applyTreeFilter()
    {
        if (!searchBox_)
            return;
        const QString query = searchBox_->text().trimmed();
        for (int index = 0; index < tree_->topLevelItemCount(); ++index)
            filterItem(tree_->topLevelItem(index), query);
    }

    void performSearch()
    {
        const QString query = searchBox_ ? searchBox_->text().trimmed() : QString();
        if (query.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("Enter text to search."), 3000);
            return;
        }
        QList<QTreeWidgetItem *> obsolete;
        for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
            QTreeWidgetItem *item = tree_->topLevelItem(index);
            if (item->data(0, Qt::UserRole).toString() == QStringLiteral("search"))
                obsolete.append(item);
        }
        for (QTreeWidgetItem *item : obsolete)
            delete tree_->takeTopLevelItem(tree_->indexOfTopLevelItem(item));

        auto *results = new QTreeWidgetItem(tree_, {QStringLiteral("Search results: %1").arg(query)});
        results->setData(0, Qt::UserRole, QStringLiteral("search"));
        int count = 0;
        const auto addResult = [&results, &count](const QString &label, const QString &target) {
            auto *item = new QTreeWidgetItem(results, {label});
            item->setData(0, Qt::UserRole, target);
            ++count;
        };
        for (int bookIndex = 0; bookIndex < books_.size(); ++bookIndex) {
            const Book &book = books_.at(bookIndex);
            for (const Article &article : book.articles) {
                QString haystack = article.title + QLatin1Char(' ') + article.section + QLatin1Char(' ') + article.category;
                for (int index = article.firstFragment; index < article.afterLastFragment && index < book.fragments.size(); ++index)
                    haystack += QLatin1Char(' ') + book.fragments.at(index).text;
                if (haystack.contains(query, Qt::CaseInsensitive))
                    addResult(QStringLiteral("[%1] %2 / %3").arg(issueMonthLabel(article.date), book.name, article.title),
                              QStringLiteral("article:") + articleKey(book.name, article.id));
            }
            for (const Article &article : book.catalogArticles) {
                if (articleLocations_.contains(articleKey(book.name, article.id)))
                    continue;
                const QString haystack = article.title + QLatin1Char(' ') + article.section + QLatin1Char(' ')
                                         + article.category + QLatin1Char(' ') + article.keywords;
                if (haystack.contains(query, Qt::CaseInsensitive))
                    addResult(QStringLiteral("[%1] %2 / %3 (DBF catalog)")
                                  .arg(issueMonthLabel(article.date), book.name, article.title),
                              QStringLiteral("catalog:") + articleKey(book.name, article.id));
            }
            if (book.articles.isEmpty()) {
                QString haystack;
                for (const Fragment &fragment : book.fragments)
                    haystack += QLatin1Char(' ') + fragment.text;
                if (haystack.contains(query, Qt::CaseInsensitive))
                    addResult(book.name + QStringLiteral(" / Recovered text"),
                              QStringLiteral("booktext:") + book.name);
            }
        }
        results->setText(0, QStringLiteral("Search results: %1 (%2)").arg(query).arg(count));
        results->setExpanded(true);
        statusBar()->showMessage(QStringLiteral("Found %1 matching page(s). ").arg(count), 4000);
    }

    void loadDirectory(const QString &directory)
    {
        const QString archiveDirectory = QDir(directory).absolutePath();
        QFile manifestFile(QDir(archiveDirectory).filePath(QStringLiteral("manifest.json")));
        if (!manifestFile.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(this, QStringLiteral("MYCOM Viewer"),
                                 QStringLiteral("The selected directory has no readable manifest.json."));
            return;
        }
        const QJsonDocument manifestDocument = QJsonDocument::fromJson(manifestFile.readAll());
        const ArchiveManifestPaths manifestPaths = manifestDocument.isObject()
            ? readArchiveManifestPaths(manifestDocument.object()) : ArchiveManifestPaths{};
        if (!manifestPaths.error.isEmpty() || !manifestDocument.isObject()) {
            QMessageBox::warning(this, QStringLiteral("MYCOM Viewer"),
                                 manifestPaths.error.isEmpty()
                                     ? QStringLiteral("The selected directory has an invalid MYCOM archive manifest.")
                                     : manifestPaths.error);
            return;
        }
        const QString contentRelative = manifestPaths.contentDirectory;
        const QString contentDirectory = QDir(archiveDirectory).filePath(contentRelative);
        if (!QFileInfo(contentDirectory).isDir()) {
            QMessageBox::warning(this, QStringLiteral("MYCOM Viewer"),
                                 QStringLiteral("The archive content directory is missing: %1").arg(contentRelative));
            return;
        }
        const QString assetRelative = manifestPaths.assetDirectory;
        const QString assetDirectory = QDir(archiveDirectory).filePath(assetRelative);
        if (!QFileInfo(assetDirectory).isDir()) {
            QMessageBox::warning(this, QStringLiteral("MYCOM Viewer"),
                                 QStringLiteral("The archive normalized asset directory is missing: %1").arg(assetRelative));
            return;
        }
        QVector<Book> loaded;
        QStringList archiveSignatureParts;
        QDirIterator iterator(contentDirectory, {QStringLiteral("*.json")}, QDir::Files, QDirIterator::NoIteratorFlags);
        while (iterator.hasNext()) {
            const QString path = iterator.next();
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                continue;
            const QJsonDocument json = QJsonDocument::fromJson(file.readAll());
            if (!json.isObject() || json.object().value(QStringLiteral("format")).toString()
                                     != QStringLiteral("mycom-mvb-salvage/v1"))
                continue;
            const QJsonObject object = json.object();
            Book book;
            book.name = QFileInfo(path).completeBaseName().toUpper();
            archiveSignatureParts.append(book.name + QLatin1Char('|')
                                         + object.value(QStringLiteral("sourceFile")).toString()
                                         + QLatin1Char('|')
                                         + QString::number(object.value(QStringLiteral("sourceBytes")).toDouble(), 'f', 0));
            for (const QJsonValue &value : object.value(QStringLiteral("textFragments")).toArray()) {
                const QJsonObject fragment = value.toObject();
                book.fragments.append({static_cast<qint64>(fragment.value(QStringLiteral("offset")).toDouble()),
                                       fragment.value(QStringLiteral("text")).toString()});
            }
            for (const QJsonValue &value : object.value(QStringLiteral("articleTopics")).toArray()) {
                const QJsonObject topic = value.toObject();
                Article article;
                article.id = topic.value(QStringLiteral("id")).toString();
                article.firstFragment = topic.value(QStringLiteral("firstFragment")).toInt();
                article.afterLastFragment = topic.value(QStringLiteral("afterLastFragment")).toInt();
                const QJsonObject metadata = topic.value(QStringLiteral("metadata")).toObject();
                article.title = metadata.value(QStringLiteral("title")).toString(article.id);
                article.date = metadata.value(QStringLiteral("date")).toString();
                article.section = metadata.value(QStringLiteral("section")).toString();
                article.category = metadata.value(QStringLiteral("category")).toString();
                article.keywords = metadata.value(QStringLiteral("keywords")).toString();
                book.articles.append(article);
            }
            for (const QJsonValue &value : object.value(QStringLiteral("catalogArticles")).toArray()) {
                const QJsonObject metadata = value.toObject();
                Article article;
                article.id = metadata.value(QStringLiteral("id")).toString();
                article.title = metadata.value(QStringLiteral("title")).toString(article.id);
                article.date = metadata.value(QStringLiteral("date")).toString();
                article.section = metadata.value(QStringLiteral("section")).toString();
                article.category = metadata.value(QStringLiteral("category")).toString();
                article.keywords = metadata.value(QStringLiteral("keywords")).toString();
                if (!article.id.isEmpty())
                    book.catalogArticles.append(article);
            }
            for (const QJsonValue &value : object.value(QStringLiteral("monthIndexes")).toArray())
                book.monthIndexes.append(value.toString());
            for (const QJsonValue &value : object.value(QStringLiteral("mediaReferences")).toArray())
                book.media.append(value.toString());
            for (const QJsonValue &value : object.value(QStringLiteral("availableMediaAssets")).toArray())
                book.availableMedia.append(value.toString());
            for (const QJsonValue &value : object.value(QStringLiteral("missingMediaReferences")).toArray())
                book.missingMedia.append(value.toString());
            if (!book.fragments.isEmpty())
                loaded.append(book);
        }

        if (loaded.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("MYCOM Viewer"),
                                 QStringLiteral("No converted MVB JSON files were found in the archive content directory."));
            return;
        }

        rootDirectory_ = QDir(contentDirectory).absolutePath();
        assetDirectory_ = QDir(assetDirectory).absolutePath();
        books_ = std::move(loaded);
        currentPageKey_.clear();
        archiveSignatureParts.sort(Qt::CaseInsensitive);
        archiveSignature_ = QString::fromLatin1(QCryptographicHash::hash(
            archiveSignatureParts.join(QLatin1Char('\n')).toUtf8(), QCryptographicHash::Sha256).toHex());
        articleLocations_.clear();
        crossBookArticleLocations_.clear();
        catalogLocations_.clear();
        bookLocations_.clear();
        mediaFiles_.clear();
        QDirIterator mediaIterator(assetDirectory_, {QStringLiteral("*.wav"), QStringLiteral("*.WAV"),
                                                     QStringLiteral("*.avi"), QStringLiteral("*.AVI")},
                                  QDir::Files, QDirIterator::Subdirectories);
        while (mediaIterator.hasNext()) {
            const QString path = mediaIterator.next();
            mediaFiles_.insert(QFileInfo(path).fileName().toLower(), path);
        }
        tree_->clear();
        QSet<QString> availableMedia;
        QSet<QString> missingMedia;
        for (int bookIndex = 0; bookIndex < books_.size(); ++bookIndex) {
            const Book &book = books_.at(bookIndex);
            bookLocations_.insert(book.name, bookIndex);
            auto *bookItem = new QTreeWidgetItem(tree_, {book.name});
            bookItem->setData(0, Qt::UserRole, QStringLiteral("book:") + book.name);

            if (!book.monthIndexes.isEmpty()) {
                auto *monthsItem = new QTreeWidgetItem(bookItem, {QStringLiteral("Monthly indexes")});
                for (const QString &month : book.monthIndexes) {
                    auto *monthItem = new QTreeWidgetItem(monthsItem, {monthLabel(month)});
                    monthItem->setData(0, Qt::UserRole, QStringLiteral("month:") + book.name + QLatin1Char('/') + month);
                }
            }

            if (!book.articles.isEmpty()) {
                auto *articlesItem = new QTreeWidgetItem(bookItem, {QStringLiteral("Articles")});
                for (int articleIndex = 0; articleIndex < book.articles.size(); ++articleIndex) {
                    const Article &article = book.articles.at(articleIndex);
                    auto *articleItem = new QTreeWidgetItem(articlesItem, {article.title});
                    articleItem->setToolTip(0, article.id);
                    articleItem->setData(0, Qt::UserRole, QStringLiteral("article:") + articleKey(book.name, article.id));
                    articleLocations_.insert(articleKey(book.name, article.id), {bookIndex, articleIndex});
                }
            } else {
                auto *recoveredItem = new QTreeWidgetItem(bookItem, {QStringLiteral("Recovered text")});
                recoveredItem->setData(0, Qt::UserRole, QStringLiteral("booktext:") + book.name);
            }
            for (int catalogIndex = 0; catalogIndex < book.catalogArticles.size(); ++catalogIndex) {
                const Article &article = book.catalogArticles.at(catalogIndex);
                catalogLocations_.insert(articleKey(book.name, article.id), {bookIndex, catalogIndex});
            }
            for (const QString &media : book.availableMedia) availableMedia.insert(media);
            for (const QString &media : book.missingMedia) missingMedia.insert(media);
        }
        // A uniquely reconstructed article may live in another MVB book than
        // its catalog/month entry.  Keep only unique IDs, so a catalog link is
        // never redirected when multiple books provide competing content.
        QSet<QString> ambiguousCrossBookIds;
        for (int bookIndex = 0; bookIndex < books_.size(); ++bookIndex) {
            const Book &book = books_.at(bookIndex);
            for (const Article &article : book.articles) {
                const QString key = articleKey(book.name, article.id);
                if (articleLocations_.contains(articleKey(QStringLiteral("HEADA"), article.id)))
                    continue;
                if (crossBookArticleLocations_.contains(article.id)) {
                    crossBookArticleLocations_.remove(article.id);
                    ambiguousCrossBookIds.insert(article.id);
                } else if (!ambiguousCrossBookIds.contains(article.id)) {
                    crossBookArticleLocations_.insert(article.id, key);
                }
            }
        }
        if (!availableMedia.isEmpty()) {
            auto *mediaItem = new QTreeWidgetItem(tree_, {QStringLiteral("Media available in ISO")});
            for (const QString &media : availableMedia) {
                auto *item = new QTreeWidgetItem(mediaItem, {media});
                item->setData(0, Qt::UserRole, QStringLiteral("media:GLOBAL/") + media);
            }
        }
        if (!missingMedia.isEmpty()) {
            auto *missingItem = new QTreeWidgetItem(tree_, {QStringLiteral("Unavailable legacy media references")});
            for (const QString &media : missingMedia) {
                auto *item = new QTreeWidgetItem(missingItem, {media});
                item->setToolTip(0, QStringLiteral("Referenced by the MVB but not present in the source ISO."));
                item->setDisabled(true);
            }
        }
        loadBookmarks();
        rebuildBookmarks();
        applyTreeFilter();
        tree_->expandToDepth(1);
        statusBar()->showMessage(QStringLiteral("Loaded %1 book(s) from %2").arg(books_.size()).arg(archiveDirectory));
    }

    void openItem(QTreeWidgetItem *item)
    {
        if (!item)
            return;
        const QString value = item->data(0, Qt::UserRole).toString();
        if (value.startsWith(QStringLiteral("article:"))) {
            showArticle(value.mid(8));
        } else if (value.startsWith(QStringLiteral("catalog:"))) {
            showCatalogArticle(value.mid(8));
        } else if (value.startsWith(QStringLiteral("month:"))) {
            showMonth(value.mid(6));
        } else if (value.startsWith(QStringLiteral("media:"))) {
            playMedia(value.mid(6));
        } else if (value.startsWith(QStringLiteral("booktext:"))) {
            showRecoveredBook(value.mid(9));
        }
    }

    void openLink(const QUrl &url)
    {
        if (url.scheme() != QStringLiteral("mycom"))
            return;
        const QStringList parts = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (url.host() == QStringLiteral("article") && parts.size() == 2)
            showArticle(parts.at(0).toUpper() + QLatin1Char('/') + parts.at(1));
        else if (url.host() == QStringLiteral("catalog") && parts.size() == 2)
            showCatalogArticle(parts.at(0).toUpper() + QLatin1Char('/') + parts.at(1));
        else if (url.host() == QStringLiteral("month") && parts.size() == 2)
            showMonth(parts.at(0).toUpper() + QLatin1Char('/') + parts.at(1));
    }

    QString topicLink(const QString &text, const QString &book, const QString &currentId) const
    {
        const QRegularExpression articleExpression(
            QStringLiteral("(?:PopupID|JumpID)\\(\\s*[`']?([A-Za-z0-9_]+)[^,]*,\\s*[`']?h([0-9]{8})>mokcha"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch articleMatch = articleExpression.match(text);
        if (articleMatch.hasMatch()) {
            const QString targetBook = articleMatch.captured(1).toUpper();
            const QString targetId = articleMatch.captured(2);
            if (targetBook == book && targetId == currentId)
                return {};
            if (articleLocations_.contains(articleKey(targetBook, targetId)))
                return QStringLiteral("mycom://article/") + targetBook + QLatin1Char('/') + targetId;
        }
        const QRegularExpression monthExpression(
            QStringLiteral("JumpID\\(\\s*[`']?([A-Za-z0-9_]+)[^,]*,\\s*[`']?(Y[0-9]{4})[`']"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch monthMatch = monthExpression.match(text);
        if (monthMatch.hasMatch())
            return QStringLiteral("mycom://month/") + monthMatch.captured(1).toUpper()
                   + QLatin1Char('/') + monthMatch.captured(2).toUpper();
        return {};
    }

    void showArticle(const QString &key)
    {
        if (!articleLocations_.contains(key))
            return;
        const auto location = articleLocations_.value(key);
        const Book &book = books_.at(location.first);
        const Article &article = book.articles.at(location.second);
        QString html = contentStyle();
        html += QStringLiteral("<h1>") + article.title.toHtmlEscaped() + QStringLiteral("</h1><p><small>")
                + article.id.toHtmlEscaped();
        if (!article.date.isEmpty()) html += QStringLiteral(" &middot; ") + article.date.toHtmlEscaped();
        if (!article.section.isEmpty()) html += QStringLiteral(" &middot; ") + article.section.toHtmlEscaped();
        if (!article.category.isEmpty()) html += QStringLiteral(" / ") + article.category.toHtmlEscaped();
        html += QStringLiteral("</small></p>");

        const QRegularExpression imageExpression(QStringLiteral("\\bbmp[\\\\/][A-Za-z0-9_-]+\\.(?:dib|bmp|shg)"),
                                                 QRegularExpression::CaseInsensitiveOption);
        bool previousWasImage = false;
        for (int index = article.firstFragment; index < article.afterLastFragment && index < book.fragments.size(); ++index) {
            const Fragment &fragment = book.fragments.at(index);
            const QRegularExpressionMatch imageMatch = imageExpression.match(fragment.text);
            const QString link = topicLink(fragment.text, book.name, article.id);
            if (imageMatch.hasMatch()) {
                QString reference = imageMatch.captured();
                reference.replace('\\', '/');
                if (isDecorativeImageReference(reference)) {
                    previousWasImage = false;
                    continue;
                }
                const QString asset = QDir(assetDirectory_).filePath(reference);
                const QString image = QUrl::fromLocalFile(asset).toString();
                html += imageHtml(reference, image, link);
                previousWasImage = true;
            } else if (!link.isEmpty()) {
                html += QStringLiteral("<p><a href=\"") + link + QStringLiteral("\">Open linked topic</a></p>");
                previousWasImage = false;
            } else {
                const bool heading = isHeadingLike(fragment.text);
                const bool figureCaption = previousWasImage && heading && fragment.text.simplified().size() <= 100;
                html += QStringLiteral("<") + (figureCaption ? QStringLiteral("p")
                                                             : (heading ? QStringLiteral("h2") : QStringLiteral("p")))
                        + QStringLiteral(" class=\"")
                        + (figureCaption ? QStringLiteral("figure-caption")
                                         : (heading ? QStringLiteral("article-heading") : QStringLiteral("article-text")))
                        + QStringLiteral("\" data-offset=\"") + QString::number(fragment.offset)
                        + QStringLiteral("\">") + escapedParagraph(fragment.text)
                        + QStringLiteral("</") + (figureCaption ? QStringLiteral("p")
                                                                  : (heading ? QStringLiteral("h2") : QStringLiteral("p"))) + QStringLiteral(">");
                previousWasImage = false;
            }
        }
        browser_->setHtml(html);
        currentPageKey_ = QStringLiteral("article:") + key;
        updateBookmarkActions();
        statusBar()->showMessage(book.name + QLatin1Char('/') + article.id);
    }

    void showMonth(const QString &key)
    {
        const QStringList parts = key.split(QLatin1Char('/'));
        if (parts.size() != 2)
            return;
        const QString prefix = monthLabel(parts.at(1));
        QString html = QStringLiteral("<h1>") + prefix + QStringLiteral(" article index</h1><ul>");
        for (const Book &book : books_) {
            if (book.name != parts.at(0))
                continue;
            const QVector<Article> &entries = book.catalogArticles.isEmpty() ? book.articles : book.catalogArticles;
            for (const Article &article : entries) {
                if (!article.date.startsWith(prefix))
                    continue;
                const QString articleKeyValue = articleKey(book.name, article.id);
                const QString resolvedArticleKey = articleLocations_.contains(articleKeyValue)
                    ? articleKeyValue : crossBookArticleLocations_.value(article.id);
                const bool reconstructed = !resolvedArticleKey.isEmpty();
                html += QStringLiteral("<li><a href=\"mycom://")
                        + (reconstructed ? QStringLiteral("article/") : QStringLiteral("catalog/"))
                        + (reconstructed ? resolvedArticleKey : articleKeyValue) + QStringLiteral("\">")
                        + article.title.toHtmlEscaped() + QStringLiteral("</a>");
                if (reconstructed && resolvedArticleKey != articleKeyValue)
                    html += QStringLiteral(" <small>(recovered from ")
                            + resolvedArticleKey.section(QLatin1Char('/'), 0, 0).toHtmlEscaped()
                            + QStringLiteral(")</small>");
                else if (!reconstructed)
                    html += QStringLiteral(" <small>(DBF catalog record)</small>");
                html += QStringLiteral("</li>");
            }
        }
        browser_->setHtml(html + QStringLiteral("</ul>"));
        currentPageKey_.clear();
        updateBookmarkActions();
    }

    void showCatalogArticle(const QString &key)
    {
        if (articleLocations_.contains(key)) {
            showArticle(key);
            return;
        }
        if (catalogLocations_.contains(key)) {
            const auto catalogLocation = catalogLocations_.value(key);
            const Article &catalogArticle = books_.at(catalogLocation.first).catalogArticles.at(catalogLocation.second);
            const QString crossBookKey = crossBookArticleLocations_.value(catalogArticle.id);
            if (!crossBookKey.isEmpty()) {
                showArticle(crossBookKey);
                return;
            }
        }
        if (!catalogLocations_.contains(key))
            return;
        const auto location = catalogLocations_.value(key);
        const Book &book = books_.at(location.first);
        const Article &article = book.catalogArticles.at(location.second);
        QString html = contentStyle();
        html += QStringLiteral("<h1>") + article.title.toHtmlEscaped() + QStringLiteral("</h1><p><small>")
                + article.id.toHtmlEscaped();
        if (!article.date.isEmpty()) html += QStringLiteral(" &middot; ") + article.date.toHtmlEscaped();
        if (!article.section.isEmpty()) html += QStringLiteral(" &middot; ") + article.section.toHtmlEscaped();
        if (!article.category.isEmpty()) html += QStringLiteral(" / ") + article.category.toHtmlEscaped();
        html += QStringLiteral("</small></p><p class=\"article-text\"><b>DBF catalog record.</b> "
                               "This article exists in the original MYCOM catalog, but its exact MVB content boundary "
                               "has not yet been reconstructed.</p>");
        if (!article.keywords.isEmpty())
            html += QStringLiteral("<p class=\"article-text\"><b>Keywords:</b> ")
                    + article.keywords.toHtmlEscaped() + QStringLiteral("</p>");
        browser_->setHtml(html);
        currentPageKey_.clear();
        updateBookmarkActions();
        statusBar()->showMessage(book.name + QLatin1Char('/') + article.id + QStringLiteral(" DBF catalog record"));
    }

    void showRecoveredBook(const QString &bookName)
    {
        if (!bookLocations_.contains(bookName))
            return;
        const Book &book = books_.at(bookLocations_.value(bookName));
        QString html = contentStyle();
        html += QStringLiteral("<h1>") + book.name.toHtmlEscaped()
                + QStringLiteral("</h1><p><small>Recovered text; no article boundaries were identified in this MVB.</small></p>");
        const QRegularExpression imageExpression(QStringLiteral("\\bbmp[\\\\/][A-Za-z0-9_-]+\\.(?:dib|bmp|shg)"),
                                                 QRegularExpression::CaseInsensitiveOption);
        bool previousWasImage = false;
        for (const Fragment &fragment : book.fragments) {
            const QRegularExpressionMatch imageMatch = imageExpression.match(fragment.text);
            if (imageMatch.hasMatch()) {
                QString reference = imageMatch.captured();
                reference.replace('\\', '/');
                if (isDecorativeImageReference(reference)) {
                    previousWasImage = false;
                    continue;
                }
                const QString asset = QDir(assetDirectory_).filePath(reference);
                html += imageHtml(reference, QUrl::fromLocalFile(asset).toString(), {});
                previousWasImage = true;
                continue;
            }
            const bool heading = isHeadingLike(fragment.text);
            const bool figureCaption = previousWasImage && heading && fragment.text.simplified().size() <= 100;
            html += QStringLiteral("<") + (figureCaption ? QStringLiteral("p")
                                                         : (heading ? QStringLiteral("h2") : QStringLiteral("p")))
                    + QStringLiteral(" class=\"")
                    + (figureCaption ? QStringLiteral("figure-caption")
                                     : (heading ? QStringLiteral("article-heading") : QStringLiteral("article-text")))
                    + QStringLiteral("\" data-offset=\"") + QString::number(fragment.offset)
                    + QStringLiteral("\">") + escapedParagraph(fragment.text)
                    + QStringLiteral("</") + (figureCaption ? QStringLiteral("p")
                                                              : (heading ? QStringLiteral("h2") : QStringLiteral("p"))) + QStringLiteral(">");
            previousWasImage = false;
        }
        browser_->setHtml(html);
        currentPageKey_ = QStringLiteral("booktext:") + book.name;
        updateBookmarkActions();
        statusBar()->showMessage(book.name + QStringLiteral(" recovered text"));
    }

    void playMedia(const QString &key)
    {
        const int separator = key.indexOf(QLatin1Char('/'));
        if (separator < 0)
            return;
        const QString reference = key.mid(separator + 1);
        const QString path = mediaFiles_.value(QFileInfo(reference).fileName().toLower());
        if (path.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("MYCOM Viewer"),
                                 QStringLiteral("The referenced media file is not present in the converted archive: %1")
                                     .arg(reference));
            return;
        }
        if (QFileInfo(path).suffix().compare(QStringLiteral("wav"), Qt::CaseInsensitive) == 0) {
            player_->setMedia(QUrl::fromLocalFile(path));
            player_->play();
            statusBar()->showMessage(QStringLiteral("Playing ") + reference);
            return;
        }
        auto *dialog = new QDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(QStringLiteral("MYCOM video: ") + reference);
        dialog->resize(800, 600);
        auto *layout = new QVBoxLayout(dialog);
        auto *video = new QVideoWidget(dialog);
        layout->addWidget(video);
        auto *videoPlayer = new QMediaPlayer(dialog);
        videoPlayer->setVideoOutput(video);
        connect(videoPlayer, QOverload<QMediaPlayer::Error>::of(&QMediaPlayer::error), dialog,
                [this, reference, videoPlayer](QMediaPlayer::Error) {
                    statusBar()->showMessage(QStringLiteral("Cannot decode ") + reference + QStringLiteral(": ")
                                             + videoPlayer->errorString(), 8000);
                });
        videoPlayer->setMedia(QUrl::fromLocalFile(path));
        dialog->show();
        videoPlayer->play();
        statusBar()->showMessage(QStringLiteral("Playing ") + reference + QStringLiteral(" in MYCOM Viewer"));
    }
};

} // namespace

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("MYCOM Archive"));
    QCoreApplication::setApplicationName(QStringLiteral("mycom-viewer"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.7.0"));
    application.setWindowIcon(QIcon(QStringLiteral(":/icons/mycom-viewer.svg")));
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("archive-directory"),
                                 QStringLiteral("Directory generated by mycom-archive-build."));
    parser.process(application);
    const QStringList positional = parser.positionalArguments();
    Viewer viewer(positional.isEmpty() ? QString() : positional.first());
    viewer.show();
    return application.exec();
}
