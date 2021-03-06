#ifndef MTX_MKVTOOLNIX_GUI_MAIN_WINDOW_UPDATE_CHECK_THREAD_H
#define MTX_MKVTOOLNIX_GUI_MAIN_WINDOW_UPDATE_CHECK_THREAD_H

#include "common/common_pch.h"

#if defined(HAVE_CURL_EASY_H)
# include <QObject>
# include <QThread>

# include "common/version.h"

namespace mtx { namespace gui {

enum class UpdateCheckStatus {
  Failed,
  NewReleaseAvailable,
  NoNewReleaseAvailable,
};

class UpdateCheckThread : public QThread {
  Q_OBJECT;

public:
  UpdateCheckThread(QObject *parent);
  virtual void run() override;

signals:
  void checkStarted();
  void checkFinished(mtx::gui::UpdateCheckStatus status, mtx_release_version_t release);
  void releaseInformationRetrieved(std::shared_ptr<pugi::xml_document> releasesInfo);
};

}}

Q_DECLARE_METATYPE(mtx::gui::UpdateCheckStatus);
Q_DECLARE_METATYPE(mtx_release_version_t);
Q_DECLARE_METATYPE(std::shared_ptr<pugi::xml_document>);

#endif  // HAVE_CURL_EASY_H
#endif  // MTX_MKVTOOLNIX_GUI_MAIN_WINDOW_UPDATE_CHECK_THREAD_H
