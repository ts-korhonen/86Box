/*
 * 86Box A hypervisor and IBM PC system emulator that specializes in
 *      running old operating systems and software designed for IBM
 *      PC systems and compatibles from 1981 through fairly recent
 *      system designs based on the PCI bus.
 *
 *      This file is part of the 86Box distribution.
 *
 *      OpenGL renderer options for Qt
 *
 * Authors:
 *      Teemu Korhonen
 *
 *      Copyright 2022 Teemu Korhonen
 */

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringBuilder>
#include <QStringList>

#include <stdexcept>

#include "qt_opengloptions.hpp"

extern "C" {
#include <86box/86box.h>
}

/* Default vertex shader. */
static const GLchar *vertex_shader = "\
in vec2 VertexCoord;\n\
in vec2 TexCoord;\n\
out vec2 tex;\n\
void main(){\n\
    gl_Position = vec4(VertexCoord, 0.0, 1.0);\n\
    tex = TexCoord;\n\
}\n";

/* Default fragment shader. */
static const GLchar *fragment_shader = "\
in vec2 tex;\n\
uniform sampler2D texsampler;\n\
out vec4 color;\n\
void main() {\n\
    color = texture(texsampler, tex);\n\
}\n";

OpenGLOptions::OpenGLOptions(QObject *parent, bool loadConfig, const QString &glslVersion)
    : QObject(parent)
    , m_glslVersion(glslVersion)
{
    m_filter = video_filter_method == 0
        ? FilterType::Nearest
        : FilterType::Linear;

    if (!loadConfig)
        return;

    /* Initialize with config. */
    m_vsync     = video_vsync != 0;
    m_framerate = video_framerate;

    m_renderBehavior = video_framerate == -1
        ? RenderBehaviorType::SyncWithVideo
        : RenderBehaviorType::TargetFramerate;

    QString shaderPath(video_shader);

    if (shaderPath.isEmpty()) {
        addDefaultShader();
    } else {
        try {
            addShader(shaderPath);
        } catch (const std::runtime_error &) {
            /* Fallback to default shader */
            addDefaultShader();
        }
    }
}

void
OpenGLOptions::save() const
{
    video_vsync         = m_vsync ? 1 : 0;
    video_framerate     = m_renderBehavior == RenderBehaviorType::SyncWithVideo ? -1 : m_framerate;
    video_filter_method = m_filter == FilterType::Nearest ? 0 : 1;

    /* TODO: multiple shaders */
    auto path = m_shaders.first().path().toLocal8Bit();

    if (!path.isEmpty())
        qstrncpy(video_shader, path.constData(), sizeof(video_shader));
    else
        video_shader[0] = '\0';
}

OpenGLOptions::FilterType
OpenGLOptions::filter() const
{
    /* Filter method is controlled externally */
    return video_filter_method == 0
        ? FilterType::Nearest
        : FilterType::Linear;
}

void
OpenGLOptions::setRenderBehavior(RenderBehaviorType value)
{
    m_renderBehavior = value;
}

void
OpenGLOptions::setFrameRate(int value)
{
    m_framerate = value;
}

void
OpenGLOptions::setVSync(bool value)
{
    m_vsync = value;
}

void
OpenGLOptions::setFilter(FilterType value)
{
    m_filter = value;
}

void
OpenGLOptions::addShader(const QString &path)
{
    auto bytes = readTextFile(path);

    auto json = QJsonDocument::fromJson(bytes);

    if (!json.isNull()) {
        for (auto &&item : json["shaders"].toArray()) {
            auto shader = item.toObject();

            auto sPath = shader["path"].toString();

            if (sPath.isEmpty())
                throw std::runtime_error("");

            QList<QPair<QString, float>> parameters;

            auto params = shader["parameters"].toObject();

            for (auto &&key : params.keys()) {

                auto param = params[key];

                parameters << qMakePair(key, param.toVariant().toFloat());
            }

            addShader(QString(readTextFile(sPath)), sPath, parameters);
        }
    } else {
        addShader(QString(bytes), path);
    }
}

void
OpenGLOptions::addShader(QString source, const QString &path, QList<QPair<QString, float>> parameters)
{
    /* Parser for parameter lines with format:
     * #pragma parameter IDENTIFIER "DESCRIPTION" INITIAL MINIMUM MAXIMUM [STEP]
     */
    QRegularExpression parameter(
        "^\\s*#pragma\\s+parameter\\s+(\\w+)\\s+\"(.+)\"\\s+(-?[\\.\\d]+)\\s+(-?[\\.\\d]+)\\s+(-?[\\.\\d]+)(\\s+-?[\\.\\d]+)?.*?\\n",
        QRegularExpression::MultilineOption);

    // auto parameters = parameter.globalMatch(source);

    // while (parameters.hasNext()) {
    //     auto param = parameters.next();

    //    qDebug() << "Name: " << param.captured(1);
    //    qDebug() << "Desc: " << param.captured(2);
    //    qDebug() << "Init: " << param.captured(3).toFloat();
    //    qDebug() << "Min : " << param.captured(4).toFloat();
    //    qDebug() << "Max : " << param.captured(5).toFloat();
    //    if (!param.captured(6).isEmpty())
    //        qDebug() << "Step: " << param.captured(6).toFloat();
    //}

    /* Remove parameter lines */
    // shader_text.remove(QRegularExpression("^\\s*#pragma parameter.*?\\n", QRegularExpression::MultilineOption));
    source.remove(parameter);

    QRegularExpression version("^\\s*(#version\\s+\\w+)", QRegularExpression::MultilineOption);

    auto match = version.match(source);

    QString version_line(m_glslVersion);

    if (match.hasMatch()) {
        /* Extract existing version and remove it. */
        version_line = match.captured(1);
        source.remove(version);
    }

    auto shader = new QOpenGLShaderProgram(this);

    auto throw_shader_error = [path, shader](const QString &what) {
        throw std::runtime_error(
            QString(what % ":\n\n %2")
                .arg(path)
                .arg(shader->log())
                .toStdString());
    };

    QStringList header {
        version_line,
        "#extension GL_ARB_shading_language_420pack : enable",
        "#define PARAMETER_UNIFORM",
        "#define %1",
        "#line 1",
        ""
    };
    QString prefix = header.join('\n');

    if (!shader->addShaderFromSourceCode(QOpenGLShader::Vertex, prefix.arg("VERTEX") % source))
        throw_shader_error(tr("Error compiling vertex shader in file \"%1\""));

    if (!shader->addShaderFromSourceCode(QOpenGLShader::Fragment, prefix.arg("FRAGMENT") % source))
        throw_shader_error(tr("Error compiling fragment shader in file \"%1\""));

    if (!shader->link())
        throw_shader_error(tr("Error linking shader program in file \"%1\""));

    m_shaders << OpenGLShaderPass(shader, path, parameters);
}

void
OpenGLOptions::addDefaultShader()
{
    auto shader = new QOpenGLShaderProgram(this);
    shader->addShaderFromSourceCode(QOpenGLShader::Vertex, m_glslVersion % "\n" % vertex_shader);
    shader->addShaderFromSourceCode(QOpenGLShader::Fragment, m_glslVersion % "\n" % fragment_shader);
    shader->link();
    m_shaders << OpenGLShaderPass(shader, QString(), QList<QPair<QString, float>>());
}

QByteArray
OpenGLOptions::readTextFile(const QString &path)
{
    QFile file(path);

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        throw std::runtime_error(
            QString(tr("Error opening \"%1\": %2"))
                .arg(path)
                .arg(file.errorString())
                .toStdString());
    }

    auto bytes = file.readAll();

    file.close();

    return bytes;
}
