/*
 * dmabuf_view.cpp -- qemu's dmabuf frames drawn by GL; see dmabuf_view.h.
 * Imported with EGL_EXT_image_dma_buf_import into the widget's own
 * context, as a texture drawn over the widget with its aspect kept.
 */

#include "dmabuf_view.h"

#include <QOpenGLShaderProgram>

#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

/* DRM's "no modifier said": the buffer's layout is the driver's guess. */
static const quint64 modifier_invalid = 0x00ffffffffffffffull;

typedef void (*target_texture_fn)(GLenum target, void *image);

static const char vertex_source[] =
    "attribute vec2 at;\n"
    "attribute vec2 from;\n"
    "varying vec2 coordinate;\n"
    "void main() { coordinate = from; gl_Position = vec4(at, 0.0, 1.0); }\n";

static const char fragment_source[] =
    "#ifdef GL_ES\n"
    "precision mediump float;\n"
    "#endif\n"
    "uniform sampler2D frame;\n"
    "varying vec2 coordinate;\n"
    "void main() { gl_FragColor = vec4(texture2D(frame, coordinate).rgb, 1.0); }\n";

dmabuf_view::dmabuf_view(QWidget *parent)
    : QOpenGLWidget(parent)
{
	setAttribute(Qt::WA_TransparentForMouseEvents);
	setFocusPolicy(Qt::NoFocus);
}

dmabuf_view::~dmabuf_view()
{
	makeCurrent();
	release();
	if (texture != 0u)
		glDeleteTextures(1, &texture);
	delete program;
	doneCurrent();
	if (fd >= 0)
		::close(fd);
	for (const auto &done : pending)
		done();
}

void dmabuf_view::initializeGL()
{
	initializeOpenGLFunctions();
	glGenTextures(1, &texture);
	program = new QOpenGLShaderProgram;
	if (!program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertex_source) ||
	    !program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragment_source) ||
	    !program->link())
		why = QStringLiteral("the frame's shader: %1").arg(program->log());
}

void dmabuf_view::release()
{
	if (image != nullptr) {
		auto destroy = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));

		if (destroy != nullptr)
			destroy(eglGetCurrentDisplay(), image);
		image = nullptr;
	}
}

void dmabuf_view::scanout(int fd_, int width, int height, int stride_, unsigned fourcc_,
                          quint64 modifier_, bool y0_top_)
{
	if (fd >= 0)
		::close(fd);
	fd = fd_;
	width_ = width;
	height_ = height;
	stride = stride_;
	fourcc = fourcc_;
	modifier = modifier_;
	y0_top = y0_top_;
	fresh = true;
	update();
}

void dmabuf_view::changed(std::function<void()> done)
{
	pending.append(std::move(done));
	update();
}

/* The buffer as an EGL image bound to the texture; false with `why`. */
bool dmabuf_view::import()
{
	EGLDisplay display = eglGetCurrentDisplay();
	auto create = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
	auto target = reinterpret_cast<target_texture_fn>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
	EGLint attributes[20];
	int    n = 0;

	release();
	fresh = false;
	if (display == EGL_NO_DISPLAY || create == nullptr || target == nullptr) {
		why = QStringLiteral("Qt's GL here is not EGL, which a 3D screen's frames need");
		return false;
	}
	attributes[n++] = EGL_WIDTH;
	attributes[n++] = width_;
	attributes[n++] = EGL_HEIGHT;
	attributes[n++] = height_;
	attributes[n++] = EGL_LINUX_DRM_FOURCC_EXT;
	attributes[n++] = (EGLint)fourcc;
	attributes[n++] = EGL_DMA_BUF_PLANE0_FD_EXT;
	attributes[n++] = fd;
	attributes[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
	attributes[n++] = 0;
	attributes[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
	attributes[n++] = stride;
	if (modifier != modifier_invalid) {
		attributes[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
		attributes[n++] = (EGLint)(modifier & 0xffffffffu);
		attributes[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
		attributes[n++] = (EGLint)(modifier >> 32);
	}
	attributes[n++] = EGL_NONE;
	image = create(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes);
	if (image == EGL_NO_IMAGE_KHR) {
		image = nullptr;
		why = QStringLiteral("the frame could not be imported: EGL error 0x%1")
		          .arg(eglGetError(), 0, 16);
		return false;
	}
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	target(GL_TEXTURE_2D, image);
	why.clear();
	return true;
}

void dmabuf_view::paintGL()
{
	const qreal scale = devicePixelRatioF();
	const int   w = (int)(width() * scale), h = (int)(height() * scale);

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	if (fresh && fd >= 0)
		import();
	if (image != nullptr && program != nullptr && program->isLinked() && width_ > 0 && height_ > 0) {
		/* The frame's aspect kept, centred, in clip space. */
		const QSize shown = QSize(width_, height_).scaled(w, h, Qt::KeepAspectRatio);
		const GLfloat x = (GLfloat)shown.width() / (GLfloat)w;
		const GLfloat y = (GLfloat)shown.height() / (GLfloat)h;
		const GLfloat at[] = { -x, -y, x, -y, -x, y, x, y };
		const GLfloat top = y0_top ? 0.0f : 1.0f, bottom = y0_top ? 1.0f : 0.0f;
		const GLfloat from[] = { 0.0f, bottom, 1.0f, bottom, 0.0f, top, 1.0f, top };

		program->bind();
		program->setUniformValue("frame", 0);
		program->enableAttributeArray("at");
		program->enableAttributeArray("from");
		program->setAttributeArray("at", at, 2);
		program->setAttributeArray("from", from, 2);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, texture);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		program->disableAttributeArray("at");
		program->disableAttributeArray("from");
		program->release();
	}
	/* qemu renders into the buffer again only once told it was drawn. */
	glFinish();
	const QList<std::function<void()>> done = std::move(pending);
	pending.clear();
	for (const auto &call : done)
		call();
}
