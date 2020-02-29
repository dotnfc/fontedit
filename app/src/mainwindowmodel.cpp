#include "mainwindowmodel.h"
#include "sourcecoderunnable.h"
#include "f2b_qt_compat.h"
#include <f2b.h>

#include <QDebug>
#include <QThreadPool>
#include <QFile>
#include <QDataStream>
#include <QDir>

#include <iostream>
#include <thread>

Q_DECLARE_METATYPE(SourceCodeOptions::BitNumbering);

namespace SettingsKey {
static const QString bitNumbering = "source_code_options/bit_numbering";
static const QString invertBits = "source_code_options/invert_bits";
static const QString includeLineSpacing = "source_code_options/include_line_spacing";
static const QString format = "source_code_options/format";
static const QString indentation = "source_code_options/indentation";
static const QString documentPath = "source_code_options/document_path";
static const QString lastDocumentDirectory = "source_code_options/last_document_directory";
static const QString lastSourceCodeDirectory = "source_code_options/last_source_code_directory";
}

bool operator==(const UIState& lhs, const UIState& rhs)
{
    return lhs.actions == rhs.actions &&
            lhs.lastUserAction == rhs.lastUserAction &&
            lhs.selectedTab == rhs.selectedTab;
}

bool operator!=(const UIState& lhs, const UIState& rhs)
{
    return !(lhs == rhs);
}

MainWindowModel::MainWindowModel(QObject *parent) :
    QObject(parent)
{
    sourceCodeOptions_.bit_numbering =
            qvariant_cast<SourceCodeOptions::BitNumbering>(
                settings_.value(SettingsKey::bitNumbering, SourceCodeOptions::LSB)
                );
    sourceCodeOptions_.invert_bits = settings_.value(SettingsKey::invertBits, false).toBool();
    sourceCodeOptions_.include_line_spacing = settings_.value(SettingsKey::includeLineSpacing, false).toBool();
    sourceCodeOptions_.indentation = from_qvariant(settings_.value(SettingsKey::indentation, to_qvariant(SourceCode::Tab {})));

    formats_.insert(QString::fromStdString(std::string(Format::C::identifier)), "C/C++");
    formats_.insert(QString::fromStdString(std::string(Format::Arduino::identifier)), "Arduino");
    formats_.insert(QString::fromStdString(std::string(Format::PythonList::identifier)), "Python List");
    formats_.insert(QString::fromStdString(std::string(Format::PythonBytes::identifier)), "Python Bytes");

    currentFormat_ = settings_.value(SettingsKey::format, formats_.firstKey()).toString();

    indentationStyles_.push_back({ SourceCode::Tab {}, tr("Tab") });
    for (std::size_t i = 1; i <= 8; ++i) {
        indentationStyles_.push_back({ SourceCode::Space {i}, tr("%n Space(s)", "", i) });
    }

    connect(this, &MainWindowModel::runnableFinished,
            this, &MainWindowModel::sourceCodeChanged,
            Qt::BlockingQueuedConnection);

    qDebug() << "output format:" << currentFormat_;
}

void MainWindowModel::restoreSession()
{
    auto path = settings_.value(SettingsKey::documentPath).toString();
    if (!path.isNull()) {
        openDocument(path, true);
    } else {
        updateDocumentTitle();
    }
}

void MainWindowModel::registerInputEvent(InputEvent e)
{
    auto state { uiState_ };
    if (std::holds_alternative<UIState::InterfaceAction>(e)) {
        auto action = std::get<UIState::InterfaceAction>(e);
        switch (action) {
        case UIState::ActionTabEdit:
            state.selectedTab = UIState::TabEdit;
            break;
        case UIState::ActionTabCode:
            state.selectedTab = UIState::TabCode;
            break;
        default:
            break;
        }
    } else if (std::holds_alternative<UIState::UserAction>(e)) {
        auto action = std::get<UIState::UserAction>(e);
        state.lastUserAction = action;
        switch (action) {
        case UIState::UserIdle:
            state.actions.reset();
            break;
        case UIState::UserLoadedFace:
            state.actions.reset();
            state.actions.set(UIState::ActionAddGlyph);
            state.actions.set(UIState::ActionSave);
            state.actions.set(UIState::ActionClose);
            state.actions.set(UIState::ActionPrint);
            state.actions.set(UIState::ActionExport);
            state.actions.set(UIState::ActionTabCode);
            break;
        case UIState::UserLoadedGlyph:
            state.actions.set(UIState::ActionCopy);
            break;
        }
        state.actions.set(UIState::ActionTabEdit);
    }

    if (state != uiState_) {
        uiState_ = state;
        emit uiStateChanged(uiState_);
    }
}

void MainWindowModel::updateDocumentTitle()
{
    QString name;
    if (documentPath_.has_value()) {
        QFileInfo fileInfo {documentPath_.value()};
        name = fileInfo.fileName();
    } else {
        name = tr("New Document");
    }

    if (auto faceModel = fontFaceViewModel_.get()) {
        if (faceModel->isModifiedSinceSave()) {
            name += " - ";
            name += tr("Edited");
        }
    }
    if (name != documentTitle_) {
        documentTitle_ = name;
        emit documentTitleChanged(documentTitle_);
    }
}

void MainWindowModel::importFont(const QFont &font)
{
    fontFaceViewModel_ = std::make_unique<FontFaceViewModel>(font);
    registerInputEvent(UIState::UserLoadedFace);
    setDocumentPath({});
    emit faceLoaded(fontFaceViewModel_->face());
    updateDocumentTitle();
}

void MainWindowModel::openDocument(const QString &fileName)
{
    openDocument(fileName, false);
}

void MainWindowModel::openDocument(const QString &fileName, bool failSilently)
{
    try {
        fontFaceViewModel_ = std::make_unique<FontFaceViewModel>(fileName);

        qDebug() << "face loaded from" << fileName;

        registerInputEvent(UIState::UserLoadedFace);
        setDocumentPath(fileName);
        updateDocumentTitle();
        emit faceLoaded(fontFaceViewModel_->face());

    } catch (std::runtime_error& e) {
        setDocumentPath({});
        updateDocumentTitle();

        qCritical() << e.what();

        if (!failSilently) {
            emit documentError(QString::fromStdString(e.what()));
        }
    }
}

void MainWindowModel::saveDocument(const QString& fileName)
{
    try {
        fontFaceViewModel_->saveToFile(fileName);

        qDebug() << "face saved to" << fileName;

        setDocumentPath(fileName);
        updateDocumentTitle();
    } catch (std::runtime_error& e) {
        qCritical() << e.what();
        emit documentError(QString::fromStdString(e.what()));
    }
}

void MainWindowModel::closeCurrentDocument()
{
    fontFaceViewModel_.release();
    setDocumentPath({});
    updateDocumentTitle();
    registerInputEvent(UIState::UserIdle);
    emit documentClosed();
}

void MainWindowModel::setActiveGlyphIndex(std::size_t index)
{
    if (fontFaceViewModel_->activeGlyphIndex().has_value() and
            fontFaceViewModel_->activeGlyphIndex().value() == index)
    {
        return;
    }

    try {
        fontFaceViewModel_->setActiveGlyphIndex(index);
        registerInputEvent(UIState::UserLoadedGlyph);

        emit activeGlyphChanged(fontFaceViewModel_->activeGlyph());
    } catch (const std::exception& e) {
        qCritical() << e.what();
    }
}

void MainWindowModel::setInvertBits(bool enabled)
{
    sourceCodeOptions_.invert_bits = enabled;
    settings_.setValue(SettingsKey::invertBits, enabled);
    reloadSourceCode();
}

void MainWindowModel::setMSBEnabled(bool enabled)
{
    auto bitNumbering = enabled ? SourceCodeOptions::MSB : SourceCodeOptions::LSB;
    sourceCodeOptions_.bit_numbering = bitNumbering;
    settings_.setValue(SettingsKey::bitNumbering, bitNumbering);
    reloadSourceCode();
}

void MainWindowModel::setIncludeLineSpacing(bool enabled)
{
    sourceCodeOptions_.include_line_spacing = enabled;
    settings_.setValue(SettingsKey::includeLineSpacing, enabled);
    reloadSourceCode();
}

void MainWindowModel::setOutputFormat(const QString& format)
{
    currentFormat_ = formats_.key(format, formats_.first());
    settings_.setValue(SettingsKey::format, currentFormat_);
    reloadSourceCode();
}

void MainWindowModel::setIndentation(const QString &indentationLabel)
{
    auto i = std::find_if(indentationStyles_.cbegin(), indentationStyles_.cend(), [&](const auto& pair) -> bool {
        return pair.second == indentationLabel;
    });
    if (i != indentationStyles_.end()) {
        sourceCodeOptions_.indentation = i->first;
        settings_.setValue(SettingsKey::indentation, to_qvariant(i->first));
        reloadSourceCode();
    }
}

QString MainWindowModel::indentationStyleCaption() const
{
    auto i = std::find_if(indentationStyles_.cbegin(), indentationStyles_.cend(), [&](const auto& pair) -> bool {
        return pair.first == sourceCodeOptions_.indentation;
    });
    if (i != indentationStyles_.end()) {
        return i->second;
    }
    return indentationStyles_.front().second;
}

void MainWindowModel::setDocumentPath(const std::optional<QString>& path)
{
    documentPath_ = path;
    if (path.has_value()) {
        settings_.setValue(SettingsKey::documentPath, path.value());
        setLastVisitedDirectory(path.value());
    } else {
        settings_.remove(SettingsKey::documentPath);
    }
}

QString MainWindowModel::lastVisitedDirectory() const
{
    return settings_.value(SettingsKey::lastDocumentDirectory).toString();
}

void MainWindowModel::setLastVisitedDirectory(const QString& path)
{
    settings_.setValue(SettingsKey::lastDocumentDirectory, QFileInfo(path).path());
}

QString MainWindowModel::lastSourceCodeDirectory() const
{
    return settings_.value(SettingsKey::lastSourceCodeDirectory).toString();
}

void MainWindowModel::setLastSourceCodeDirectory(const QString& path)
{
    settings_.setValue(SettingsKey::lastSourceCodeDirectory, QFileInfo(path).path());
}

void MainWindowModel::reloadSourceCode()
{
    /// WIP :)
    emit sourceCodeUpdating();

    auto r = new SourceCodeRunnable { faceModel()->face(), sourceCodeOptions_, currentFormat_, fontArrayName_ };
    r->setCompletionHandler([&](const QString& output) {
        emit runnableFinished(output);
    });
    r->setAutoDelete(true);

    QThreadPool::globalInstance()->start(r);
}
