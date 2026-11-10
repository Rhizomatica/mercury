//go:build hybris && linux

package main

// Sailfish OS's on-screen keyboard.  The phone's compositor offers no Wayland
// text-input protocol: an app reaches the keyboard (Maliit) over D-Bus, on
// maliit-server's peer-to-peer socket, whose address is the session bus's
// org.maliit.server /org/maliit/server/address property.  While a text field
// has the focus the keyboard is up, and what it types goes to that field:
// committed text as runes, Backspace, Return, Tab and the arrows as keys.
// Prediction and correction are asked off, so no preedit is shown.

import (
	"strings"
	"sync"
	"time"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/widget"
	"github.com/godbus/dbus/v5"
)

const (
	maliitServerPath   = "/com/meego/inputmethod/uiserver1"
	maliitServerIface  = "com.meego.inputmethod.uiserver1"
	maliitContextPath  = "/com/meego/inputmethod/inputcontext"
	maliitContextIface = "com.meego.inputmethod.inputcontext1"
)

// Qt's key codes, as Maliit sends them (Qt::Key).
const (
	qtKeyTab       = 0x01000001
	qtKeyBackspace = 0x01000003
	qtKeyReturn    = 0x01000004
	qtKeyEnter     = 0x01000005
	qtKeyDelete    = 0x01000007
	qtKeyLeft      = 0x01000012
	qtKeyUp        = 0x01000013
	qtKeyRight     = 0x01000014
	qtKeyDown      = 0x01000015
	qtKeyPress     = 6 // QEvent::KeyPress
)

type onScreenKeyboard struct {
	app  fyne.App
	logf func(string)

	mu      sync.Mutex
	conn    *dbus.Conn
	target  fyne.Focusable // the field the keyboard types into
	shown   bool
	userHid bool // hidden from the keyboard itself: not again for this field
}

func startOnScreenKeyboard(app fyne.App, logf func(string)) {
	k := &onScreenKeyboard{app: app, logf: logf}
	go k.watch()
}

// watch follows the focus: the text field focused in any of the app's windows.
func (k *onScreenKeyboard) watch() {
	for range time.Tick(250 * time.Millisecond) {
		var field fyne.Focusable
		fyne.DoAndWait(func() {
			for _, w := range k.app.Driver().AllWindows() {
				if f := textField(w.Canvas().Focused()); f != nil {
					field = f
					return
				}
			}
		})
		k.follow(field)
	}
}

func textField(f fyne.Focusable) fyne.Focusable {
	switch e := f.(type) {
	case *widget.Entry:
		if !e.Disabled() {
			return e
		}
	case *widget.SelectEntry:
		if !e.Disabled() {
			return e
		}
	}
	return nil
}

func (k *onScreenKeyboard) follow(field fyne.Focusable) {
	k.mu.Lock()
	defer k.mu.Unlock()
	if field != k.target {
		k.target, k.userHid = field, false
	} else if (field != nil) == k.shown || k.userHid {
		return
	}
	if field == nil {
		if k.shown {
			k.call("updateWidgetInformation", map[string]dbus.Variant{"focusState": dbus.MakeVariant(false)}, true)
			k.call("hideInputMethod")
			k.shown = false
		}
		return
	}
	info := map[string]dbus.Variant{
		"focusState":                dbus.MakeVariant(true),
		"contentType":               dbus.MakeVariant(int32(0)), // free text
		"predictionEnabled":         dbus.MakeVariant(false),
		"correctionEnabled":         dbus.MakeVariant(false),
		"autocapitalizationEnabled": dbus.MakeVariant(false),
	}
	k.shown = k.call("activateContext") &&
		k.call("updateWidgetInformation", info, true) &&
		k.call("showInputMethod")
}

// call sends one method call to maliit-server, connecting first if need be.
// The message has no destination: godbus's Object.Call always sets one, and
// on a peer-to-peer connection an empty destination makes the server drop it.
func (k *onScreenKeyboard) call(member string, args ...interface{}) bool {
	if k.conn == nil && !k.connect() {
		return false
	}
	msg := &dbus.Message{
		Type:  dbus.TypeMethodCall,
		Flags: dbus.FlagNoReplyExpected,
		Headers: map[dbus.HeaderField]dbus.Variant{
			dbus.FieldPath:      dbus.MakeVariant(dbus.ObjectPath(maliitServerPath)),
			dbus.FieldInterface: dbus.MakeVariant(maliitServerIface),
			dbus.FieldMember:    dbus.MakeVariant(member),
		},
		Body: args,
	}
	if len(args) > 0 {
		msg.Headers[dbus.FieldSignature] = dbus.MakeVariant(dbus.SignatureOf(args...))
	}
	if c := k.conn.Send(msg, nil); c.Err != nil {
		k.logf("On-screen keyboard: " + member + ": " + c.Err.Error() + "\n")
		k.conn.Close()
		k.conn, k.shown = nil, false
		return false
	}
	return true
}

func (k *onScreenKeyboard) connect() bool {
	bus, err := dbus.SessionBus()
	if err != nil {
		return false
	}
	v, err := bus.Object("org.maliit.server", "/org/maliit/server/address").
		GetProperty("org.maliit.Server.Address.address")
	addr, _ := v.Value().(string)
	if err != nil || addr == "" {
		return false
	}
	c, err := dbus.Dial(addr)
	if err == nil {
		err = c.Auth(nil)
	}
	if err == nil {
		err = c.ExportWithMap(&maliitContext{k}, map[string]string{
			"ActivationLostEvent":   "activationLostEvent",
			"ImInitiatedHide":       "imInitiatedHide",
			"CommitString":          "commitString",
			"UpdatePreedit":         "updatePreedit",
			"KeyEvent":              "keyEvent",
			"UpdateInputMethodArea": "updateInputMethodArea",
			"SetGlobalCorrection":   "setGlobalCorrectionEnabled",
			"SetRedirectKeys":       "setRedirectKeys",
			"SetDetectableRepeat":   "setDetectableAutoRepeat",
			"SetSelection":          "setSelection",
			"SetLanguage":           "setLanguage",
		}, maliitContextPath, maliitContextIface)
	}
	if err != nil {
		k.logf("On-screen keyboard: maliit-server: " + err.Error() + "\n")
		if c != nil {
			c.Close()
		}
		return false
	}
	k.conn = c
	return true
}

// deliver hands what the keyboard typed to the field the keyboard is up for, on the UI thread.
func (k *onScreenKeyboard) deliver(fn func(fyne.Focusable)) {
	k.mu.Lock()
	t := k.target
	k.mu.Unlock()
	if t != nil {
		fyne.Do(func() { fn(t) })
	}
}

// maliitContext is the input context maliit-server calls back.
type maliitContext struct{ k *onScreenKeyboard }

func (m *maliitContext) ActivationLostEvent() *dbus.Error { return nil }

func (m *maliitContext) ImInitiatedHide() *dbus.Error {
	m.k.mu.Lock()
	m.k.shown, m.k.userHid = false, true
	m.k.mu.Unlock()
	return nil
}

func (m *maliitContext) CommitString(text string, replaceStart, replaceLength, cursorPos int32) *dbus.Error {
	m.k.deliver(func(f fyne.Focusable) {
		for _, line := range strings.SplitAfter(text, "\n") {
			for _, r := range strings.TrimSuffix(line, "\n") {
				f.TypedRune(r)
			}
			if strings.HasSuffix(line, "\n") {
				f.TypedKey(&fyne.KeyEvent{Name: fyne.KeyReturn})
			}
		}
	})
	return nil
}

func (m *maliitContext) UpdatePreedit(text string, formats []struct{ A, B, C int32 },
	replaceStart, replaceLength, cursorPos int32) *dbus.Error {
	return nil
}

func (m *maliitContext) KeyEvent(typ, key, modifiers int32, text string, autoRepeat bool,
	count int32, requestType byte) *dbus.Error {
	if typ != qtKeyPress {
		return nil
	}
	names := map[int32]fyne.KeyName{
		qtKeyTab: fyne.KeyTab, qtKeyBackspace: fyne.KeyBackspace, qtKeyReturn: fyne.KeyReturn,
		qtKeyEnter: fyne.KeyEnter, qtKeyDelete: fyne.KeyDelete, qtKeyLeft: fyne.KeyLeft,
		qtKeyUp: fyne.KeyUp, qtKeyRight: fyne.KeyRight, qtKeyDown: fyne.KeyDown,
	}
	if name, ok := names[key]; ok {
		m.k.deliver(func(f fyne.Focusable) { f.TypedKey(&fyne.KeyEvent{Name: name}) })
	} else if text != "" {
		m.k.deliver(func(f fyne.Focusable) {
			for _, r := range text {
				f.TypedRune(r)
			}
		})
	}
	return nil
}

func (m *maliitContext) UpdateInputMethodArea(x, y, width, height int32) *dbus.Error { return nil }
func (m *maliitContext) SetGlobalCorrection(enabled bool) *dbus.Error                { return nil }
func (m *maliitContext) SetRedirectKeys(enabled bool) *dbus.Error                    { return nil }
func (m *maliitContext) SetDetectableRepeat(enabled bool) *dbus.Error                { return nil }
func (m *maliitContext) SetSelection(start, length int32) *dbus.Error                { return nil }
func (m *maliitContext) SetLanguage(language string) *dbus.Error                     { return nil }
