--[[
 Copyright 2019 Konsulko Group

 author:Edi Feschiyan <edi.feschiyan@konsulko.com>

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

--[[ A paired phone/device having a phonebook is needed to run the tests

--]]

_AFT.testVerbStatusSuccess('testContactsSuccess','bluetooth-pbap','contacts', {})
_AFT.testVerbStatusSuccess('testIncomingCallsEntrySuccess','bluetooth-pbap','entry', {list="ich",handle="1.vcf"})
_AFT.testVerbStatusSuccess('testOutgoingCallsEntrySuccess','bluetooth-pbap','entry', {list="och",handle="1.vcf"})
_AFT.testVerbStatusSuccess('testMissedCallsEntrySuccess','bluetooth-pbap','entry', {list="mch",handle="1.vcf"})
_AFT.testVerbStatusSuccess('testCombinedCallsEntrySuccess','bluetooth-pbap','entry', {list="cch",handle="1.vcf"})
_AFT.testVerbStatusSuccess('testPhoneBookEntrySuccess','bluetooth-pbap','entry', {list="pb",handle="1.vcf"})
_AFT.testVerbStatusSuccess('testIncomingCallsHistorySuccess','bluetooth-pbap','history', {list="ich"})
_AFT.testVerbStatusSuccess('testOutgoingCallsHistorySuccess','bluetooth-pbap','history', {list="och"})
_AFT.testVerbStatusSuccess('testMissedCallsHistorySuccess','bluetooth-pbap','history', {list="mch"})
_AFT.testVerbStatusSuccess('testCombinedCallsHistorySuccess','bluetooth-pbap','history', {list="cch"})
_AFT.testVerbStatusSuccess('testSearchSuccess','bluetooth-pbap','search', {number="100"})
_AFT.testVerbStatusSuccess('testStatusSuccess','bluetooth-pbap','status', {value="connected"})
_AFT.testVerbStatusSuccess('testSubscribeStatusSuccess','bluetooth-pbap','subscribe', {value="status"})
_AFT.testVerbStatusSuccess('testUnsubscribeStatusSuccess','bluetooth-pbap','unsubscribe', {value="status"})
