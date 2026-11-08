// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/gl/glprobe.h"

#include <algorithm>
#include <array>

#include <QCoreApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>

namespace ts
{

namespace
{

QString glString(QOpenGLFunctions* functions, GLenum name)
{
  const GLubyte* value = functions->glGetString(name);
  return value ? QString::fromLatin1(reinterpret_cast<const char*>(value))
               : QString();
}

} // namespace

bool isSoftwareRenderer(const QString& renderer)
{
  static const std::array<const char*, 8> markers = {
      "llvmpipe",    "softpipe",
      "swrast",      "Software Rasterizer",
      "SwiftShader", "Microsoft Basic Render",
      "GDI Generic", "lavapipe"};
  return std::any_of(
      markers.begin(), markers.end(), [&renderer](const char* marker) {
        return renderer.contains(QLatin1String(marker), Qt::CaseInsensitive);
      });
}

GlProbeResult probeOpenGl(int major, int minor)
{
  GlProbeResult result;

  QSurfaceFormat format;
  format.setRenderableType(QSurfaceFormat::OpenGL);
  format.setVersion(major, minor);
  format.setProfile(QSurfaceFormat::CoreProfile);

  QOpenGLContext context;
  context.setFormat(format);
  if (!context.create()) {
    result.errorString = QCoreApplication::translate(
        "ts::GlProbe", "Could not create an OpenGL context.");
    return result;
  }

  QOffscreenSurface surface;
  surface.setFormat(context.format());
  surface.create();
  if (!surface.isValid() || !context.makeCurrent(&surface)) {
    result.errorString = QCoreApplication::translate(
        "ts::GlProbe", "Could not make the OpenGL context current.");
    return result;
  }

  const QSurfaceFormat actual = context.format();
  result.majorVersion = actual.majorVersion();
  result.minorVersion = actual.minorVersion();
  result.coreProfile = actual.profile() == QSurfaceFormat::CoreProfile;

  QOpenGLFunctions* functions = context.functions();
  result.vendor = glString(functions, GL_VENDOR);
  result.renderer = glString(functions, GL_RENDERER);
  result.version = glString(functions, GL_VERSION);
  result.softwareRenderer = isSoftwareRenderer(result.renderer);

  context.doneCurrent();

  const bool versionOk =
      result.majorVersion > major ||
      (result.majorVersion == major && result.minorVersion >= minor);
  if (context.isOpenGLES()) {
    result.errorString = QCoreApplication::translate(
        "ts::GlProbe", "Got an OpenGL ES context; desktop OpenGL is needed.");
  } else if (!versionOk) {
    result.errorString =
        QCoreApplication::translate("ts::GlProbe",
                                    "OpenGL %1.%2 is needed but only %3.%4 is "
                                    "available (%5).")
            .arg(major)
            .arg(minor)
            .arg(result.majorVersion)
            .arg(result.minorVersion)
            .arg(result.renderer);
  } else {
    result.ok = true;
  }
  return result;
}

} // namespace ts
