# Audio Mixer Service

## Overview

The Audio Mixer service exposes PipeWire mixer controls to applications.

Each of the controls has a *volume* and a *mute* value. *volume* is a floating
point number between 0.0 and 1.0, while *mute* is an integer that can be
either 0 or 1.

To receive the list of controls, call the *list_controls* verb. The reply will
also include the current values for both *volume* and *mute* for each control.
When the list of controls changes, the *controls_changed* event is emited. The
receiver is then expected to invalidate its cached state of all the controls
and call *list_controls* again.

## Verbs

| Name               | Description                             | JSON Parameters                                     |
| ------------------ |:----------------------------------------|:----------------------------------------------------|
| subscribe          | subscribe to mixer events               | *Request:* {"event": "volume_changed"}              |
| unsubscribe        | unsubscribe from mixer events           | *Request:* {"event": "mute_changed"}                |
| list_controls      | list the available controls             | See the *List controls* section                     |
| volume             | get/set volume                          | See the *Get/Set Volume/Mute* section               |
| mute               | get/set mute                            | See the *Get/Set Volume/Mute* section               |

### List controls

* Request: empty
* Reply: Array of objects of the form **{"control": "Emergency", "volume": "1.0", mute: "0"}**

### Get/Set Volume/Mute

Getting a volume or a mute value can be achieved by calling the appropriate verb
with a parameter that specifies only the control name: **{"control": "Master"}**

Setting a volume or a mute value can be achived by calling the appropriate verb
with a parameter that specifies both the control name and the value that you want
to be set:
* On the *volume* verb: **{"control": "Multimedia", "value": "0.8"}**
* On the *mute* verb: **{"control": "Multimedia", "value": "1"}**

In both cases, the reply will include the current value:
* On the *volume* verb: **{"volume": "0.8"}**
* On the *mute* verb: **{"mute": "1"}**

## Events

| Name               | Description                             | JSON Payload                                        |
| ------------------ |:----------------------------------------|-----------------------------------------------------|
| controls_changed   | the list of available controls changed  | None. Call *list_controls* to get the new controls  |
| volume_changed     | volume has changed for a control        | {"control": "Multimedia", "volume": "0.65"}         |
| mute_changed       | mute has changed for a control          | {"control": "Multimedia", "mute": "1"}              |

## Environment variables

| Name               | Description                                       |
|--------------------|:--------------------------------------------------|
| XDG_RUNTIME_DIR    | the location of the pipewire socket to connect to |

## Notes regarding the connection to PipeWire

This binding is engineered in such a way so that when PipeWire exits, the
binding attempts to re-connect to it, possibly restarting PipeWire due to
socket activation. This re-connection attempt, however, is only done when a
verb is called. Therefore, if PipeWire for some reason exits and there is no
call to any verb on this binding afterwards, the binding will not emit
any events until this happens.
