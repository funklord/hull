/*
 * screen_window.h -- a machine's screen shown by hull-gui itself, as
 * VirtualBox shows a machine's, rather than handed to a VNC client:
 * qemu's D-Bus display (project.md section 5.2), its framebuffer shared
 * by a memfd where qemu offers one, so nothing is copied but the paint.
 * Keyboard and mouse go back to the guest; Ctrl+Alt+Enter toggles
 * fullscreen, VMware's chord.
 *
 * Reached by the machine's monitor, linked as
 * $XDG_RUNTIME_DIR/hull/display/ADDRESS: local machines only. A remote
 * one, or one with 3D, is still VNC's.
 */

#ifndef HULL_GUI_SCREEN_WINDOW_H
#define HULL_GUI_SCREEN_WINDOW_H

#include <QCursor>
#include <QImage>
#include <QWidget>

#include <functional>

class QTimer;
class dmabuf_view;
struct screen_link;

class screen_window : public QWidget {
	Q_OBJECT

public:
	/* The window for machine `id`, whose display's monitor is `monitor`. */
	screen_window(const QString &id, const QString &monitor, QWidget *parent = nullptr);
	~screen_window() override;

	/* Connect; false with `why` where the display cannot be reached. */
	bool open(QString *why);

	/* The frame shown, null before the first; whether it is qemu's own
	 * memory, mapped, rather than a copy. For its test. */
	QSize frame_size() const { return source_size(); }
	bool  shared() const { return map != nullptr; }
	bool  on_gpu() const { return view != nullptr; }

	/* The listener's calls, from screen_window.cpp's GLib callbacks. */
	void scanout(QImage image, void *map, size_t map_length);
	void update_rect(const QRect &rect);
	void disable();
	void lost(const QString &why);
	void cursor(const QImage &image, const QPoint &hot);
	void cursor_shown(bool on);
	void scanout_dmabuf(int fd, int width, int height, int stride, unsigned fourcc,
	                    quint64 modifier, bool y0_top);
	void update_dmabuf(std::function<void()> done);

protected:
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;
	void keyReleaseEvent(QKeyEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;
	bool focusNextPrevChild(bool next) override;

private:
	QRect shown() const;
	QSize source_size() const;
	void  key(QKeyEvent *event, bool down);
	void  button(QMouseEvent *event, bool down);
	void  tell_size();

	QString      id;
	QString      monitor;
	screen_link *link = nullptr;
	QImage       frame;
	void        *map = nullptr;      /* qemu's shared framebuffer, mapped, or null */
	size_t       map_length = 0;
	QCursor      guest_cursor;
	bool         cursor_on = true;
	dmabuf_view *view = nullptr;     /* a 3D machine's frames, or null */
	QTimer      *resized = nullptr;  /* the size told once it settles */
};

#endif
