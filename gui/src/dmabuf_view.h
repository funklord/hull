/*
 * dmabuf_view.h -- a 3D machine's frames in hull-gui's screen window:
 * qemu renders the guest's virgl or Venus output on the host's GPU and
 * hands each frame over as a dmabuf, which is imported here as an EGL
 * image and drawn as a texture, never leaving the GPU (project.md section
 * 5.2). It needs Qt's GL to be EGL, which main.cpp asks of xcb.
 */

#ifndef HULL_GUI_DMABUF_VIEW_H
#define HULL_GUI_DMABUF_VIEW_H

#include <QOpenGLFunctions>
#include <QOpenGLWidget>

#include <functional>

class QOpenGLShaderProgram;

class dmabuf_view : public QOpenGLWidget, protected QOpenGLFunctions {
	Q_OBJECT

public:
	explicit dmabuf_view(QWidget *parent = nullptr);
	~dmabuf_view() override;

	/* A new buffer, `fd` taken: drawn from the next paint on. */
	void scanout(int fd, int width, int height, int stride, unsigned fourcc, quint64 modifier,
	             bool y0_top);

	/* Its contents changed; `done` runs once they are drawn, which is when
	 * qemu may render into it again. */
	void changed(std::function<void()> done);

	QSize frame_size() const { return QSize(width_, height_); }

	/* Why the last buffer could not be shown, or empty. */
	QString failure() const { return why; }

protected:
	void initializeGL() override;
	void paintGL() override;

private:
	void release();
	bool import();

	int                    fd = -1;
	int                    width_ = 0, height_ = 0, stride = 0;
	unsigned               fourcc = 0u;
	quint64                modifier = 0u;
	bool                   y0_top = true;
	bool                   fresh = false;    /* imported at the next paint */
	void                  *image = nullptr;  /* EGLImage */
	unsigned               texture = 0u;
	QOpenGLShaderProgram  *program = nullptr;
	QString                why;
	QList<std::function<void()>> pending;    /* qemu waiting for a paint */
};

#endif
