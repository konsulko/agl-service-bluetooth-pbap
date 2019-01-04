# Bluetooth PBAP Service

## Overview

Bluetooth PBAP service reports respective vCard phonebook data from BlueZ via communication with PBAP profile

## Verbs

| Name        | Description                               | JSON Response                                      |
|-------------|-------------------------------------------|----------------------------------------------------|
| subscribe   | subscribe to Bluetooth PBAP events        | *Request:* {"value": "status"}                     |
| unsubscribe | unsubscribe to Bluetooth PBAP events      | *Request:* {"value": "status"}                     |
| contacts    | return all contacts from connected device | see **contacts verb section**                      |
| entry       | return vCard data from handle             | see **entry verb section**                         |
| history     | return call history list                  | see **history verb section**                       |
| search      | search for respective vCard handle        | see **search verb section**                        |
| status      | current device connection status          | same response as noted in **status event section** |

### contacts Verb

Returns all vCards that are accessible from respective connected device in concatenated output:

<pre>
 "response": {
     "vcards": "BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Art McGee\r\nN:Art\r\nTEL: +13305551212\r\nUID:27e\r\nEND:VCARD\r\n"
 }
</pre>

### search Verb

Example of a request for vCard search using **number** parameter (i.e. *{"number":"+15035551212"}*) results:

<pre>
 "response": {
     "results": [
           {
                "handle": "27e.vcf",
                "name": "Art McGee"
           }
     ]
 }                                       },
</pre>

### entry Verb

Client must pass one of the following values to the **list** parameter in request:

| Value         | Description                                      |
|---------------|--------------------------------------------------|
| ich           | Incoming calls                                   |
| och           | Outgoing calls                                   |
| mch           | Missed calls                                     |
| cch           | Combined calls (e.g. incoming, outgoing, missed) |
| pb            | Phonebook (typically selected)                   |

Also there is a **handle** parameter that must be in form of vCard path (e.g. 27e.vcf).

<pre>
 "response": {
     "vcard":"BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Art McGee\r\nN:;Art\r\nTEL;TYPE=CELL:+13305551212\r\nUID:27e\r\nEND:VCARD\r\n"
 }
</pre>

### history Verb

Client must pass one of the following values to the list parameter in request:

| Value         | Description                                      |
|---------------|--------------------------------------------------|
| ich           | Incoming calls                                   |
| och           | Outgoing calls                                   |
| mch           | Missed calls                                     |
| cch           | Combined calls (e.g. incoming, outgoing, missed) |

Sample request for a combined list (i.e. *{"list":"cch"}*) and its respective response:

<pre>
 "response": {
     "vcards":"BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Art \r\nN:\r\nTEL:3305551212\r\nX-IRMC-CALL-DATETIME;DIALED:20190103T202524\r\nEND:VCARD\r\nBEGIN:VCARD\r\nVERSION:3.0\r\nFN:Art McGee\r\nN:;Art\r\nTEL;TYPE=CELL:+15035551212\r\nUID:27e\r\nX-IRMC-CALL-DATETIME;RECEIVED:20181207T065311\r\nEND:VCARD\r\nBEGIN:VCARD\r\nVERSION:3.0\r\n"
 }
</pre>

## Events

Respective binding only outputs one event that signals if an PBAP capable device is connected

### status Event

Sample of a Bluetooth PBAP status event:

<pre>
{
  "connected": true
}
</pre>
