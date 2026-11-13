// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * The About window: version, licence, the data shown and where it comes
 * from, and the third-party notices.
 */
#ifndef TS_APP_ABOUTDIALOG_H
#define TS_APP_ABOUTDIALOG_H

#include <vector>

#include <QDialog>
#include <QString>

class QEvent;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTreeWidget;

namespace ts
{

/** One dataset the program can show, for the About window's Data tab. */
struct AboutDataRow {
  /** The pack it is in ("Base pack", or the pack's name). */
  QString pack;
  /** The dataset's name. */
  QString name;
  /** Where it comes from: the source product and version. */
  QString source;
  /** Its licence. */
  QString licence;
  /** What it asks to be credited with. */
  QString attribution;
};

/**
 * The About window (modal): Terra Soar's version and build, its licence
 * (GPL v2 or later) and home page; the datasets of the base pack and of
 * the installed data packs with their sources, licences and attributions;
 * and the full third-party notices and GPL text, built into the program.
 */
class AboutDialog : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(AboutDialog)

public:
  /** Creates the window listing @p rows. */
  AboutDialog(QWidget* parent, std::vector<AboutDataRow> rows);
  ~AboutDialog() override;

  /** Returns THIRD_PARTY_NOTICES, as built into the program. */
  static QString thirdPartyNotices();

  /** Returns the GPL (COPYING), as built into the program. */
  static QString licenceText();

  /** The project's home page. */
  static QString homePage();

protected:
  /** Re-applies all text on QEvent::LanguageChange. */
  void changeEvent(QEvent* event) override;

private:
  void retranslateUi();

  std::vector<AboutDataRow> m_data;
  QTabWidget* m_tabs = nullptr;
  QLabel* m_aboutLabel = nullptr;
  QTreeWidget* m_dataTree = nullptr;
  QPlainTextEdit* m_noticesText = nullptr;
  QPlainTextEdit* m_licenceText = nullptr;
  QPushButton* m_closeButton = nullptr;
};

} // namespace ts

#endif // TS_APP_ABOUTDIALOG_H
