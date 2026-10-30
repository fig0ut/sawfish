#include "profilestore.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QUrl>
#include <QUuid>

ProfileStore::ProfileStore(QString directory, QObject* parent)
    : QAbstractListModel(parent), m_directory(std::move(directory)) {}

int ProfileStore::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(m_profiles.size());
}
QVariant ProfileStore::data(const QModelIndex& index, int role) const {
    const auto* profile = at(index.row());
    if (!index.isValid() || !profile)
        return {};
    if (role == Qt::DisplayRole)
        return profile->name;
    if (role == Qt::UserRole)
        return profile->id;
    return {};
}
const Profile* ProfileStore::at(int row) const {
    return row >= 0 && row < m_profiles.size() ? &m_profiles[row] : nullptr;
}
const Profile* ProfileStore::find(const QString& id) const {
    for (const auto& profile : m_profiles)
        if (profile.id == id)
            return &profile;
    return nullptr;
}
bool ProfileStore::validUrl(const QString& value) {
    if (value.isEmpty())
        return true;
    const QUrl url(value, QUrl::StrictMode);
    return url.isValid() && !url.host().isEmpty() &&
           (url.scheme() == "https" || url.scheme() == "http");
}
bool ProfileStore::validContent(const QByteArray& bytes, QString* error) const {
    // Invalid configurations (including empty text) remain editable profiles.
    if (bytes.size() > MaxConfigBytes || QString::fromUtf8(bytes).toUtf8() != bytes ||
        bytes.contains('\0')) {
        *error = tr("Use UTF-8 configuration text no larger than 2 MiB.");
        return false;
    }
    return true;
}
bool ProfileStore::load(QString* error) {
    QFile file(m_directory + "/profiles.json");
    if (!file.exists()) {
        m_loaded = true;
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        *error = file.errorString();
        return false;
    }
    const auto bytes = file.read(32 * 1024 * 1024 + 1);
    const auto document = QJsonDocument::fromJson(bytes);
    if (bytes.size() > 32 * 1024 * 1024 || !document.isObject() ||
        document.object().value("version").toInt() != 1 ||
        !document.object().value("profiles").isArray()) {
        *error = tr("Cannot read the profile store. Its existing data has been preserved.");
        return false;
    }
    QList<Profile> profiles;
    QSet<QString> ids;
    for (const auto& value : document.object().value("profiles").toArray()) {
        const auto obj = value.toObject();
        Profile p;
        p.id = obj.value("id").toString();
        p.name = obj.value("name").toString();
        p.content = obj.value("content").toString();
        p.url = obj.value("url").toString();
        if (p.id.isEmpty() || ids.contains(p.id) || p.name.isEmpty() ||
            !obj.value("content").isString() || !validContent(p.content.toUtf8(), error) ||
            !validUrl(p.url) || profiles.size() >= 50) {
            *error =
                tr("The profile store contains an invalid entry. Its data has been preserved.");
            return false;
        }
        ids.insert(p.id);
        profiles.append(p);
    }
    beginResetModel();
    m_profiles = std::move(profiles);
    endResetModel();
    m_loaded = true;
    return true;
}
QString ProfileStore::uniqueName(QString name) const {
    if (name.trimmed().isEmpty())
        name = tr("Imported profile");
    const auto base = name;
    int suffix = 1;
    auto taken = [&] {
        for (const auto& p : m_profiles)
            if (p.name == name)
                return true;
        return false;
    };
    while (taken())
        name = QStringLiteral("%1 (%2)").arg(base).arg(suffix++);
    return name;
}
bool ProfileStore::save(const QList<Profile>& profiles, QString* error) const {
    if (!m_loaded) {
        *error = tr("Resolve the profile-store error before changing profiles.");
        return false;
    }
    QJsonArray entries;
    for (const auto& p : profiles)
        entries.append(
            QJsonObject{{"id", p.id}, {"name", p.name}, {"content", p.content}, {"url", p.url}});
    const auto encoded = QJsonDocument(QJsonObject{{"version", 1}, {"profiles", entries}}).toJson();
    if (encoded.size() > 32 * 1024 * 1024) {
        *error = tr("The profile store is full (32 MiB).");
        return false;
    }
    if (!QDir().mkpath(m_directory)) {
        *error = tr("Cannot create the profile directory.");
        return false;
    }
    QFile::setPermissions(m_directory,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QSaveFile output(m_directory + "/profiles.json");
    if (!output.open(QIODevice::WriteOnly) ||
        !output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        output.write(encoded) != encoded.size() || !output.commit()) {
        *error = output.errorString();
        return false;
    }
    return true;
}
bool ProfileStore::importFile(const QString& path, QString* id, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = file.errorString();
        return false;
    }
    return add(QFileInfo(path).fileName(), file.read(MaxConfigBytes + 1), {}, id, error);
}
bool ProfileStore::add(QString name, const QByteArray& content, const QString& url, QString* id,
                       QString* error) {
    if (m_profiles.size() >= 50) {
        *error = tr("This preview supports up to 50 profiles.");
        return false;
    }
    if (!validContent(content, error))
        return false;
    if (!validUrl(url)) {
        *error = tr("Enter a valid HTTP or HTTPS URL.");
        return false;
    }
    Profile profile;
    profile.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    profile.name = uniqueName(std::move(name));
    profile.content = QString::fromUtf8(content);
    profile.url = url;
    auto updated = m_profiles;
    updated.append(profile);
    if (!save(updated, error))
        return false;
    const int row = int(m_profiles.size());
    beginInsertRows({}, row, row);
    m_profiles = std::move(updated);
    endInsertRows();
    *id = profile.id;
    return true;
}
bool ProfileStore::update(const QString& id, QString name, const QString& url,
                          const QString& content, QString* error) {
    name = name.trimmed();
    if (!find(id)) {
        *error = tr("This profile no longer exists.");
        return false;
    }
    if (name.isEmpty()) {
        *error = tr("Enter a profile name.");
        return false;
    }
    for (const auto& p : m_profiles)
        if (p.id != id && p.name == name) {
            *error = tr("That profile name is already in use.");
            return false;
        }
    if (!validUrl(url)) {
        *error = tr("Enter a valid HTTP or HTTPS URL, or leave it empty.");
        return false;
    }
    if (!validContent(content.toUtf8(), error))
        return false;
    auto updated = m_profiles;
    for (int row = 0; row < updated.size(); ++row) {
        auto& p = updated[row];
        if (p.id != id)
            continue;
        p.name = name;
        p.url = url;
        p.content = content;
        p.validation = Profile::Pending;
        p.validationError.clear();
        if (!save(updated, error))
            return false;
        m_profiles = std::move(updated);
        emit dataChanged(index(row), index(row));
        return true;
    }
    return false;
}
bool ProfileStore::remove(const QString& id, QString* error) {
    for (int row = 0; row < m_profiles.size(); ++row) {
        if (m_profiles[row].id != id)
            continue;
        auto updated = m_profiles;
        updated.removeAt(row);
        if (!save(updated, error))
            return false;
        beginRemoveRows({}, row, row);
        m_profiles = std::move(updated);
        endRemoveRows();
        return true;
    }
    *error = tr("This profile no longer exists.");
    return false;
}
void ProfileStore::setValidation(const QString& id, const QString& content,
                                 Profile::Validation state, const QString& error) {
    for (int row = 0; row < m_profiles.size(); ++row) {
        auto& p = m_profiles[row];
        if (p.id != id || p.content != content)
            continue;
        p.validation = state;
        p.validationError = error;
        emit dataChanged(index(row), index(row));
        return;
    }
}
