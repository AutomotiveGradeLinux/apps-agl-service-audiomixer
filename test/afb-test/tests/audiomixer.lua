--[[
 Copyright 2020 Fujitsu

 author: Li Xiaoming <lixm.fnst@cn.fujitsu.com>

 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
--]]



_AFT.testVerbStatusSuccess('testListControlSuccess','audiomixer','list_controls', {})

_AFT.testVerbStatusSuccess('testMuteChange0Success','audiomixer','mute', {"control":"Master", "value": "0"})
_AFT.testVerbStatusSuccess('testMuteChange1Success','audiomixer','mute', {"control":"Master", "value": "1"})
_AFT.testVerbStatusSuccess('testMuteNotChangeSuccess','audiomixer','mute', {"control":"Master"})

_AFT.testVerbStatusSuccess('testVolumeChangeSuccess','audiomixer','volume', {"control":"Master", "value": "0.5"})
_AFT.testVerbStatusSuccess('testVolumeNotChangeSuccess','audiomixer','volume', {"control":"Master"})

_AFT.testVerbStatusSuccess('testSubscribeControls_changedSuccess','audiomixer','subscribe', {"event":"controls_changed"})
_AFT.testVerbStatusSuccess('testSubscribeVolume_changedSuccess','audiomixer','subscribe', {"event":"volume_changed"})
_AFT.testVerbStatusSuccess('testSubscribeMute_changedSuccess','audiomixer','subscribe', {"event":"mute_changed"})

_AFT.testVerbStatusSuccess('testUnSubscribeControls_changedSuccess','audiomixer','unsubscribe', {"event":"controls_changed"})
_AFT.testVerbStatusSuccess('testUnSubscribeVolume_changedSuccess','audiomixer','unsubscribe', {"event":"volume_changed"})
_AFT.testVerbStatusSuccess('testUnSubscribeMute_changedSuccess','audiomixer','unsubscribe', {"event":"mute_changed"})




