package main

import (
	"image/color"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/canvas"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/driver/desktop"
	"fyne.io/fyne/v2/widget"
)

// hoverTooltipButton wraps a *widget.Button and shows a tooltip popup while the
// pointer hovers over it.
//
// A plain wrapper does not work for a disabled button: widget.Button itself
// implements desktop.Hoverable, so it shadows the wrapper in the hover
// hit-test and the tooltip never fires.  Instead an invisible hover-catcher is
// stacked on top of the button whenever it is disabled; when the button is
// enabled the catcher is hidden so the button stays clickable and no tooltip
// is shown.
type hoverTooltipButton struct {
	widget.BaseWidget
	btn     *widget.Button
	overlay *hoverCatcher
	tooltip func() string
	popup   *widget.PopUp
}

func newHoverTooltipButton(btn *widget.Button, tooltip func() string) *hoverTooltipButton {
	h := &hoverTooltipButton{btn: btn, tooltip: tooltip}
	h.ExtendBaseWidget(h)
	h.overlay = newHoverCatcher(func() { h.show() }, func() { h.hide() })
	if btn.Disabled() {
		h.overlay.Show()
	} else {
		h.overlay.Hide()
	}
	return h
}

func (h *hoverTooltipButton) CreateRenderer() fyne.WidgetRenderer {
	return widget.NewSimpleRenderer(container.NewStack(h.btn, h.overlay))
}

func (h *hoverTooltipButton) Enable() {
	h.btn.Enable()
	h.overlay.Hide()
	h.hide()
}

func (h *hoverTooltipButton) Disable() {
	h.btn.Disable()
	h.overlay.Show()
	h.hide()
}

func (h *hoverTooltipButton) Disabled() bool {
	return h.btn.Disabled()
}

func (h *hoverTooltipButton) show() {
	text := h.tooltip()
	if text == "" {
		h.hide()
		return
	}
	if h.popup != nil {
		h.popup.Hide()
	}
	label := widget.NewLabel(text)
	h.popup = widget.NewPopUp(label, fyne.CurrentApp().Driver().CanvasForObject(h.btn))
	h.popup.ShowAtRelativePosition(fyne.NewPos(0, h.btn.Size().Height), h.btn)
}

func (h *hoverTooltipButton) hide() {
	if h.popup != nil {
		h.popup.Hide()
		h.popup = nil
	}
}

// hoverCatcher is an invisible widget that only reports hover enter/leave.
type hoverCatcher struct {
	widget.BaseWidget
	onIn  func()
	onOut func()
}

func newHoverCatcher(onIn, onOut func()) *hoverCatcher {
	c := &hoverCatcher{onIn: onIn, onOut: onOut}
	c.ExtendBaseWidget(c)
	return c
}

func (c *hoverCatcher) CreateRenderer() fyne.WidgetRenderer {
	return widget.NewSimpleRenderer(canvas.NewRectangle(color.Transparent))
}

func (c *hoverCatcher) MouseIn(*desktop.MouseEvent) { c.onIn() }

func (c *hoverCatcher) MouseMoved(*desktop.MouseEvent) {}

func (c *hoverCatcher) MouseOut() { c.onOut() }
