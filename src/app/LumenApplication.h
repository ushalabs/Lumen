#pragma once

#include <QObject>
#include <memory>

namespace lumen::indexing {
class FileIndexer;
} // namespace lumen::indexing

namespace lumen::ui {
class DesktopSearchBar;
} // namespace lumen::ui

namespace lumen::app {

class TrayController;

class LumenApplication : public QObject {
    Q_OBJECT

public:
    explicit LumenApplication(QObject* parent = nullptr);
    ~LumenApplication() override;

    bool initialize(bool startBackgroundIndexer = true);
    void shutdown();

    ui::DesktopSearchBar* desktopSearchBar() const noexcept { return m_searchBar.get(); }
    TrayController* trayController() const noexcept { return m_trayController.get(); }
    indexing::FileIndexer* fileIndexer() const noexcept { return m_fileIndexer.get(); }

private:
    std::unique_ptr<ui::DesktopSearchBar> m_searchBar;
    std::unique_ptr<indexing::FileIndexer> m_fileIndexer;
    std::unique_ptr<TrayController> m_trayController;
    bool m_initialized = false;
};

} // namespace lumen::app
