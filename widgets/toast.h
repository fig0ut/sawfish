#pragma once
#include <QPropertyAnimation>
#include <QTimer>
#include <QWidget>

class QLabel;

// A single, non-modal notice. New messages replace old ones; Logs retains history.
class Toast : public QWidget {
    Q_OBJECT
  public:
    explicit Toast(QWidget* parent);
    void present(const QString& message, bool error);

  protected:
    bool eventFilter(QObject* object, QEvent* event) override;
    void paintEvent(QPaintEvent*) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    void position();
    QLabel* m_label;
    QTimer m_timeout;
    QPropertyAnimation m_slide;
    bool m_error = false;
};
