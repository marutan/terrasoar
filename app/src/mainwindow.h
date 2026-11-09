// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * The application's main window.
 */
#ifndef TS_APP_MAINWINDOW_H
#define TS_APP_MAINWINDOW_H

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include <QMainWindow>
#include <QString>

#include "ts/packs/packmanifest.h"

class QAction;
class QEvent;
class QLabel;
class QMenu;
class QTimer;

namespace ts
{

class GlobeView;
class LayerTreeDock;
class LayerTreeModel;
class ProviderRegistry;

/**
 * The top-level window: the globe view, the layer tree dock, menus and
 * the status bar, which shows the position under the pointer and the eye
 * height. Projects (layer tree and camera) are opened and saved from the
 * File menu.
 *
 * All text is set in retranslateUi(), which runs again when the language
 * changes, so installing a translator at run time updates the window.
 *
 * @guithread
 */
class MainWindow : public QMainWindow
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(MainWindow)

public:
  /** Creates the window. @param parent Usual QWidget parent. */
  explicit MainWindow(QWidget* parent = nullptr);

  /** Destroys the window and its children. */
  ~MainWindow() override;

  /** Returns the layer tree. */
  LayerTreeModel* layerModel() const { return m_layers; }

  /**
   * Opens the project at @p path: replaces the layer tree and moves the
   * camera. Returns false and sets @p errorString on failure (the current
   * project stays).
   */
  bool openProject(const QString& path, QString* errorString);

  /**
   * Saves the layer tree and camera to @p path. Returns false and sets
   * @p errorString on failure.
   */
  bool saveProject(const QString& path, QString* errorString);

  /** Returns the path of the current project, empty if never saved. */
  const QString& projectPath() const { return m_projectPath; }

  /** What is known about the base pack. */
  enum class BasePackState {
    Missing,  ///< No base pack was found.
    Checking, ///< Found; its checksums are being verified.
    Ok,       ///< Found and intact.
    Damaged,  ///< Found, but a file is missing or changed.
  };

  /** Returns the base pack's state. */
  BasePackState basePackState() const { return m_basePackState; }

  /** Returns the base pack's directory, empty if there is none. */
  const QString& basePackDirectory() const { return m_basePackDirectory; }

signals:
  /** The base pack's checksums have been verified (see basePackState()). */
  void basePackVerified();

protected:
  /** Re-applies all text on QEvent::LanguageChange. */
  void changeEvent(QEvent* event) override;

private:
  void createActions();
  void createMenus();
  void retranslateUi();
  void showAbout();
  void showPointerPosition(bool onGlobe, double latDeg, double lonDeg);
  void showEyeHeight(double eyeHeightM);
  void refreshLayers();
  void openProjectDialog();
  void saveProjectDialog(bool askForPath);
  void findBasePack();
  void pollBasePackCheck();
  void showBasePackResult(bool ok, const QString& problem);

  /** The globe, or null where the platform has no OpenGL. */
  GlobeView* m_globeView = nullptr;
  /** Shown instead of the globe without OpenGL. */
  QLabel* m_noGlobeLabel = nullptr;
  QMenu* m_fileMenu = nullptr;
  QMenu* m_viewMenu = nullptr;
  QMenu* m_helpMenu = nullptr;
  QAction* m_quitAction = nullptr;
  QAction* m_debugOverlayAction = nullptr;
  QAction* m_resetViewAction = nullptr;
  QAction* m_openAction = nullptr;
  QAction* m_saveAction = nullptr;
  QAction* m_saveAsAction = nullptr;
  LayerTreeDock* m_layerDock = nullptr;
  QString m_projectPath;
  QAction* m_aboutAction = nullptr;
  QAction* m_aboutQtAction = nullptr;
  QLabel* m_statusLabel = nullptr;
  QLabel* m_pointerLabel = nullptr;
  QLabel* m_eyeLabel = nullptr;
  LayerTreeModel* m_layers = nullptr;
  std::unique_ptr<ProviderRegistry> m_registry;
  QString m_basePackDirectory;
  BasePackState m_basePackState = BasePackState::Missing;
  QString m_basePackProblem;
  /** Verifies the base pack off the GUI thread; joined on destruction. */
  std::thread m_basePackCheck;
  std::atomic<bool> m_cancelBasePackCheck{false};
  /** The check's result, set by its thread, taken by pollBasePackCheck(). */
  std::optional<PackVerification> m_basePackResult;
  std::mutex m_basePackMutex;
  QTimer* m_basePackPoll = nullptr;
  bool m_pointerOnGlobe = false;
  double m_pointerLatDeg = 0.0;
  double m_pointerLonDeg = 0.0;
  double m_eyeHeightM = 0.0;
};

} // namespace ts

#endif // TS_APP_MAINWINDOW_H
