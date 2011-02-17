/***************************************************************************
 *   Copyright (C) 2008 by Jean-Baptiste Mardelle (jb@kdenlive.org)        *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA          *
 ***************************************************************************/

#include "projectsettings.h"
#include "kdenlivesettings.h"
#include "profilesdialog.h"
#include "docclipbase.h"
#include "titlewidget.h"
#include "effectslist.h"

#include <KStandardDirs>
#include <KMessageBox>
#include <KDebug>
#include <kio/directorysizejob.h>
#include <KIO/NetAccess>
#include <KTemporaryFile>
#include <KFileDialog>

#include <QDir>
#include <kmessagebox.h>

ProjectSettings::ProjectSettings(ProjectList *projectlist, QStringList lumas, int videotracks, int audiotracks, const QString projectPath, bool readOnlyTracks, bool savedProject, QWidget * parent) :
    QDialog(parent), m_savedProject(savedProject), m_projectList(projectlist), m_lumas(lumas)
{
    setupUi(this);

    list_search->setTreeWidget(files_list);

    QMap <QString, QString> profilesInfo = ProfilesDialog::getProfilesInfo();
    QMapIterator<QString, QString> i(profilesInfo);
    while (i.hasNext()) {
        i.next();
        profiles_list->addItem(i.key(), i.value());
    }
    project_folder->setMode(KFile::Directory);
    project_folder->setUrl(KUrl(projectPath));
    QString currentProf = KdenliveSettings::current_profile();

    for (int i = 0; i < profiles_list->count(); i++) {
        if (profiles_list->itemData(i).toString() == currentProf) {
            profiles_list->setCurrentIndex(i);
            break;
        }
    }

    m_buttonOk = buttonBox->button(QDialogButtonBox::Ok);
    //buttonOk->setEnabled(false);
    audio_thumbs->setChecked(KdenliveSettings::audiothumbnails());
    video_thumbs->setChecked(KdenliveSettings::videothumbnails());
    audio_tracks->setValue(audiotracks);
    video_tracks->setValue(videotracks);
    connect(enable_proxy, SIGNAL(toggled(bool)), proxy_box, SLOT(setEnabled(bool)));
    connect(generate_proxy, SIGNAL(toggled(bool)), proxy_minsize, SLOT(setEnabled(bool)));
    
    if (projectlist) {
        enable_proxy->setChecked(projectlist->useProxy());
        generate_proxy->setChecked(projectlist->generateProxy());
        proxy_minsize->setValue(projectlist->proxyMinSize());
        proxy_params->setText(projectlist->proxyParams());
        proxy_box->setEnabled(projectlist->useProxy());
    }
    else {
        enable_proxy->setChecked(KdenliveSettings::enableproxy());
        generate_proxy->setChecked(KdenliveSettings::generateproxy());
        proxy_minsize->setValue(KdenliveSettings::proxyminsize());
        proxy_params->setText(KdenliveSettings::proxyparams());
        proxy_box->setEnabled(KdenliveSettings::enableproxy());
    }
    
    if (readOnlyTracks) {
        video_tracks->setEnabled(false);
        audio_tracks->setEnabled(false);
    }
    slotUpdateDisplay();
    if (m_projectList != NULL) {
        slotUpdateFiles();
        connect(clear_cache, SIGNAL(clicked()), this, SLOT(slotClearCache()));
        connect(delete_unused, SIGNAL(clicked()), this, SLOT(slotDeleteUnused()));
    } else tabWidget->widget(1)->setEnabled(false);
    connect(profiles_list, SIGNAL(currentIndexChanged(int)), this, SLOT(slotUpdateDisplay()));
    connect(project_folder, SIGNAL(textChanged(const QString &)), this, SLOT(slotUpdateButton(const QString &)));
    connect(button_export, SIGNAL(clicked()), this, SLOT(slotExportToText()));
}

void ProjectSettings::slotDeleteUnused()
{
    QStringList toDelete;
    QList <DocClipBase*> list = m_projectList->documentClipList();
    for (int i = 0; i < list.count(); i++) {
        DocClipBase *clip = list.at(i);
        if (clip->numReferences() == 0 && clip->clipType() != SLIDESHOW) {
            KUrl url = clip->fileURL();
            if (!url.isEmpty() && !toDelete.contains(url.path())) toDelete << url.path();
        }
    }

    // make sure our urls are not used in another clip
    for (int i = 0; i < list.count(); i++) {
        DocClipBase *clip = list.at(i);
        if (clip->numReferences() > 0) {
            KUrl url = clip->fileURL();
            if (!url.isEmpty() && toDelete.contains(url.path())) toDelete.removeAll(url.path());
        }
    }

    if (toDelete.count() == 0) {
        // No physical url to delete, we only remove unused clips from project (color clips for example have no physical url)
        if (KMessageBox::warningContinueCancel(this, i18n("This will remove all unused clips from your project."), i18n("Clean up project")) == KMessageBox::Cancel) return;
        m_projectList->cleanup();
        slotUpdateFiles();
        return;
    }
    if (KMessageBox::warningYesNoList(this, i18n("This will remove the following files from your hard drive.\nThis action cannot be undone, only use if you know what you are doing.\nAre you sure you want to continue?"), toDelete, i18n("Delete unused clips")) != KMessageBox::Yes) return;
    m_projectList->trashUnusedClips();
    slotUpdateFiles();
}

void ProjectSettings::slotClearCache()
{
    buttonBox->setEnabled(false);
    KIO::NetAccess::del(KUrl(project_folder->url().path(KUrl::AddTrailingSlash) + "thumbs/"), this);
    KStandardDirs::makeDir(project_folder->url().path(KUrl::AddTrailingSlash) + "thumbs/");
    buttonBox->setEnabled(true);
    slotUpdateFiles(true);
}

void ProjectSettings::slotUpdateFiles(bool cacheOnly)
{
    KIO::DirectorySizeJob * job = KIO::directorySize(project_folder->url().path(KUrl::AddTrailingSlash) + "thumbs/");
    job->exec();
    thumbs_count->setText(QString::number(job->totalFiles()));
    thumbs_size->setText(KIO::convertSize(job->totalSize()));
    delete job;
    if (cacheOnly) return;
    int unused = 0;
    int used = 0;
    KIO::filesize_t usedSize = 0;
    KIO::filesize_t unUsedSize = 0;
    QList <DocClipBase*> list = m_projectList->documentClipList();
    files_list->clear();

    // List all files that are used in the project. That also means:
    // images included in slideshow and titles, files in playlist clips
    // TODO: images used in luma transitions, files used for LADSPA effects?

    // Setup categories
    QTreeWidgetItem *videos = new QTreeWidgetItem(files_list, QStringList() << i18n("Video clips"));
    videos->setIcon(0, KIcon("video-x-generic"));
    videos->setExpanded(true);
    QTreeWidgetItem *sounds = new QTreeWidgetItem(files_list, QStringList() << i18n("Audio clips"));
    sounds->setIcon(0, KIcon("audio-x-generic"));
    sounds->setExpanded(true);
    QTreeWidgetItem *images = new QTreeWidgetItem(files_list, QStringList() << i18n("Image clips"));
    images->setIcon(0, KIcon("image-x-generic"));
    images->setExpanded(true);
    QTreeWidgetItem *slideshows = new QTreeWidgetItem(files_list, QStringList() << i18n("Slideshow clips"));
    slideshows->setIcon(0, KIcon("image-x-generic"));
    slideshows->setExpanded(true);
    QTreeWidgetItem *texts = new QTreeWidgetItem(files_list, QStringList() << i18n("Text clips"));
    texts->setIcon(0, KIcon("text-plain"));
    texts->setExpanded(true);
    QTreeWidgetItem *others = new QTreeWidgetItem(files_list, QStringList() << i18n("Other clips"));
    others->setIcon(0, KIcon("unknown"));
    others->setExpanded(true);
    int count = 0;
    QStringList allFonts;
    foreach(const QString & file, m_lumas) {
        count++;
        new QTreeWidgetItem(images, QStringList() << file);
    }

    for (int i = 0; i < list.count(); i++) {
        DocClipBase *clip = list.at(i);
        if (clip->clipType() == SLIDESHOW) {
            QStringList subfiles = extractSlideshowUrls(clip->fileURL());
            foreach(const QString & file, subfiles) {
                count++;
                new QTreeWidgetItem(slideshows, QStringList() << file);
            }
        } else if (!clip->fileURL().isEmpty()) {
            //allFiles.append(clip->fileURL().path());
            switch (clip->clipType()) {
            case TEXT:
                new QTreeWidgetItem(texts, QStringList() << clip->fileURL().path());
                break;
            case AUDIO:
                new QTreeWidgetItem(sounds, QStringList() << clip->fileURL().path());
                break;
            case IMAGE:
                new QTreeWidgetItem(images, QStringList() << clip->fileURL().path());
                break;
            case UNKNOWN:
                new QTreeWidgetItem(others, QStringList() << clip->fileURL().path());
                break;
            default:
                new QTreeWidgetItem(videos, QStringList() << clip->fileURL().path());
                break;
            }
            count++;
        }
        if (clip->clipType() == TEXT) {
            QStringList imagefiles = TitleWidget::extractImageList(clip->getProperty("xmldata"));
            QStringList fonts = TitleWidget::extractFontList(clip->getProperty("xmldata"));
            foreach(const QString & file, imagefiles) {
                count++;
                new QTreeWidgetItem(images, QStringList() << file);
            }
            allFonts << fonts;
        } else if (clip->clipType() == PLAYLIST) {
            QStringList files = extractPlaylistUrls(clip->fileURL().path());
            foreach(const QString & file, files) {
                count++;
                new QTreeWidgetItem(others, QStringList() << file);
            }
        }

        if (clip->numReferences() == 0) {
            unused++;
            unUsedSize += clip->fileSize();
        } else {
            used++;
            usedSize += clip->fileSize();
        }
    }
#if QT_VERSION >= 0x040500
    allFonts.removeDuplicates();
#endif
    // Hide unused categories
    for (int i = 0; i < files_list->topLevelItemCount(); i++) {
        if (files_list->topLevelItem(i)->childCount() == 0) {
            files_list->topLevelItem(i)->setHidden(true);
        }
    }
    files_count->setText(QString::number(count));
    fonts_list->addItems(allFonts);
    if (allFonts.isEmpty()) {
        fonts_list->setHidden(true);
        label_fonts->setHidden(true);
    }
    used_count->setText(QString::number(used));
    used_size->setText(KIO::convertSize(usedSize));
    unused_count->setText(QString::number(unused));
    unused_size->setText(KIO::convertSize(unUsedSize));
    delete_unused->setEnabled(unused > 0);
}

void ProjectSettings::accept()
{
    if (!m_savedProject && selectedProfile() != KdenliveSettings::current_profile())
        if (KMessageBox::warningContinueCancel(this, i18n("Changing the profile of your project cannot be undone.\nIt is recommended to save your project before attempting this operation that might cause some corruption in transitions.\n Are you sure you want to proceed?"), i18n("Confirm profile change")) == KMessageBox::Cancel) return;
    QDialog::accept();
}

void ProjectSettings::slotUpdateDisplay()
{
    QString currentProfile = profiles_list->itemData(profiles_list->currentIndex()).toString();
    QMap< QString, QString > values = ProfilesDialog::getSettingsFromFile(currentProfile);
    p_size->setText(values.value("width") + 'x' + values.value("height"));
    p_fps->setText(values.value("frame_rate_num") + '/' + values.value("frame_rate_den"));
    p_aspect->setText(values.value("sample_aspect_num") + '/' + values.value("sample_aspect_den"));
    p_display->setText(values.value("display_aspect_num") + '/' + values.value("display_aspect_den"));
    if (values.value("progressive").toInt() == 0) {
        p_progressive->setText(i18n("Interlaced (%1 fields per second)",
                                    QString::number((double)2 * values.value("frame_rate_num").toInt() / values.value("frame_rate_den").toInt(), 'f', 2)));
    } else {
        p_progressive->setText(i18n("Progressive"));
    }
    p_colorspace->setText(ProfilesDialog::getColorspaceDescription(values.value("colorspace").toInt()));
}

void ProjectSettings::slotUpdateButton(const QString &path)
{
    if (path.isEmpty()) m_buttonOk->setEnabled(false);
    else {
        m_buttonOk->setEnabled(true);
        slotUpdateFiles(true);
    }
}

QString ProjectSettings::selectedProfile() const
{
    return profiles_list->itemData(profiles_list->currentIndex()).toString();
}

KUrl ProjectSettings::selectedFolder() const
{
    return project_folder->url();
}

QPoint ProjectSettings::tracks()
{
    QPoint p;
    p.setX(video_tracks->value());
    p.setY(audio_tracks->value());
    return p;
}

bool ProjectSettings::enableVideoThumbs() const
{
    return video_thumbs->isChecked();
}

bool ProjectSettings::enableAudioThumbs() const
{
    return audio_thumbs->isChecked();
}

bool ProjectSettings::useProxy() const
{
    return enable_proxy->isChecked();
}

bool ProjectSettings::generateProxy() const
{
    return generate_proxy->isChecked();
}

int ProjectSettings::proxyMinSize() const
{
    return proxy_minsize->value();
}

QString ProjectSettings::proxyParams() const
{
    return proxy_params->toPlainText();
}

//static
QStringList ProjectSettings::extractPlaylistUrls(QString path)
{
    QStringList urls;
    QDomDocument doc;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return urls;
    if (!doc.setContent(&file)) {
        file.close();
        return urls;
    }
    file.close();
    QString root = doc.documentElement().attribute("root");
    if (!root.isEmpty() && !root.endsWith('/')) root.append('/');
    QDomNodeList files = doc.elementsByTagName("producer");
    for (int i = 0; i < files.count(); i++) {
        QDomElement e = files.at(i).toElement();
        QString type = EffectsList::property(e, "mlt_service");
        if (type != "colour") {
            QString url = EffectsList::property(e, "resource");
            if (!url.isEmpty()) {
                if (!url.startsWith('/')) url.prepend(root);
                if (url.section('.', 0, -2).endsWith("/.all")) {
                    // slideshow clip, extract image urls
                    urls << extractSlideshowUrls(KUrl(url));
                } else urls << url;
                if (url.endsWith(".mlt") || url.endsWith(".kdenlive")) {
                    //TODO: Do something to avoid infinite loops if 2 files reference themselves...
                    urls << extractPlaylistUrls(url);
                }
            }
        }
    }

    // luma files for transitions
    files = doc.elementsByTagName("transition");
    for (int i = 0; i < files.count(); i++) {
        QDomElement e = files.at(i).toElement();
        QString url = EffectsList::property(e, "luma");
        if (!url.isEmpty()) {
            if (!url.startsWith('/')) url.prepend(root);
            urls << url;
        }
    }

    return urls;
}


//static
QStringList ProjectSettings::extractSlideshowUrls(KUrl url)
{
    QStringList urls;
    QString path = url.directory(KUrl::AppendTrailingSlash);
    QString ext = url.path().section('.', -1);
    QDir dir(path);
    if (url.path().contains(".all.")) {
        // this is a mime slideshow, like *.jpeg
        QStringList filters;
        filters << "*." + ext;
        dir.setNameFilters(filters);
        QStringList result = dir.entryList(QDir::Files);
        urls.append(path + filters.at(0) + " (" + i18np("1 image found", "%1 images found", result.count()) + ")");
    } else {
        // this is a pattern slideshow, like sequence%4d.jpg
        QString filter = url.fileName();
        QString ext = filter.section('.', -1);
        filter = filter.section('%', 0, -2);
        QString regexp = "^" + filter + "\\d+\\." + ext + "$";
        QRegExp rx(regexp);
        int count = 0;
        QStringList result = dir.entryList(QDir::Files);
        foreach(const QString & path, result) {
            if (rx.exactMatch(path)) count++;
        }
        urls.append(url.path() + " (" + i18np("1 image found", "%1 images found", count) + ")");
    }
    return urls;
}

void ProjectSettings::slotExportToText()
{
    QString savePath = KFileDialog::getSaveFileName(project_folder->url(), "text/plain", this);
    if (savePath.isEmpty()) return;
    QString data;
    data.append(i18n("Project folder: %1",  project_folder->url().path()) + "\n");
    data.append(i18n("Project profile: %1",  profiles_list->currentText()) + "\n");
    data.append(i18n("Total clips: %1 (%2 used in timeline).", files_count->text(), used_count->text()) + "\n\n");
    for (int i = 0; i < files_list->topLevelItemCount(); i++) {
        if (files_list->topLevelItem(i)->childCount() > 0) {
            data.append("\n" + files_list->topLevelItem(i)->text(0) + ":\n\n");
            for (int j = 0; j < files_list->topLevelItem(i)->childCount(); j++) {
                data.append(files_list->topLevelItem(i)->child(j)->text(0) + "\n");
            }
        }
    }
    KTemporaryFile tmpfile;
    if (!tmpfile.open()) {
        kWarning() << "/////  CANNOT CREATE TMP FILE in: " << tmpfile.fileName();
        return;
    }
    QFile xmlf(tmpfile.fileName());
    xmlf.open(QIODevice::WriteOnly);
    xmlf.write(data.toUtf8());
    if (xmlf.error() != QFile::NoError) {
        xmlf.close();
        return;
    }
    xmlf.close();
    KIO::NetAccess::upload(tmpfile.fileName(), savePath, 0);
}



#include "projectsettings.moc"


