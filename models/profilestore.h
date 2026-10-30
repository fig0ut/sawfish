#pragma once
#include <QAbstractListModel>
struct Profile {
    enum Validation { Pending, Checking, Valid, Invalid };
    QString id, name, content, url;
    Validation validation = Pending;
    QString validationError;
};
class ProfileStore : public QAbstractListModel {
    Q_OBJECT
  public:
    static constexpr qint64 MaxConfigBytes = 2 * 1024 * 1024;
    explicit ProfileStore(QString directory, QObject* parent = nullptr);
    bool load(QString* error);
    bool importFile(const QString& file, QString* id, QString* error);
    bool add(QString name, const QByteArray& content, const QString& url, QString* id,
             QString* error);
    bool update(const QString& id, QString name, const QString& url, const QString& content,
                QString* error);
    bool remove(const QString& id, QString* error);
    void setValidation(const QString& id, const QString& content, Profile::Validation state,
                       const QString& error = {});
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    const Profile* find(const QString& id) const;
    const Profile* at(int row) const;
    static bool validUrl(const QString& url);

  private:
    bool save(const QList<Profile>& profiles, QString* error) const;
    bool validContent(const QByteArray& content, QString* error) const;
    QString uniqueName(QString name) const;
    QString m_directory;
    QList<Profile> m_profiles;
    bool m_loaded = false;
};
