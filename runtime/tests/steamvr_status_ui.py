import argparse
import base64
import json
import os
import socket
import struct
import urllib.request

parser = argparse.ArgumentParser(
    description="Verify status labels with the running SteamVR renderer."
)
parser.add_argument("--schema", required=True)
parser.add_argument("--port", type=int, default=8087)
parser.add_argument("--expect-running", action="store_true")
args = parser.parse_args()
pages = json.load(
    urllib.request.urlopen(f"http://127.0.0.1:{args.port}/json/list", timeout=5)
)
page = next(p for p in pages if p["title"] == "systemui")
sock = socket.create_connection(("127.0.0.1", args.port), timeout=5)
key = base64.b64encode(os.urandom(16)).decode()
sock.sendall(
    (
        f"GET /devtools/page/{page['id']} HTTP/1.1\r\nHost: 127.0.0.1:{args.port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
    ).encode()
)
header = b""
while not header.endswith(b"\r\n\r\n"):
    header += sock.recv(1)
assert header.startswith(b"HTTP/1.1 101 "), header


def exact(size):
    result = b""
    while len(result) < size:
        chunk = sock.recv(size - len(result))
        if not chunk:
            raise EOFError()
        result += chunk
    return result


sequence = 0


def call(method, params=None):
    global sequence
    sequence += 1
    payload = json.dumps(
        {"id": sequence, "method": method, "params": params or {}}
    ).encode()
    mask = os.urandom(4)
    length = len(payload)
    prefix = (
        bytes([0x81, 0x80 | length])
        if length < 126
        else bytes([0x81, 0xFE]) + struct.pack("!H", length)
    )
    sock.sendall(prefix + mask + bytes(c ^ mask[i % 4] for i, c in enumerate(payload)))
    while True:
        first, second = exact(2)
        size = second & 127
        if size == 126:
            size = struct.unpack("!H", exact(2))[0]
        elif size == 127:
            size = struct.unpack("!Q", exact(8))[0]
        data = exact(size)
        if first & 15 != 1:
            continue
        message = json.loads(data)
        if message.get("id") == sequence:
            return message


with open(args.schema, encoding="utf-8") as source:
    section = json.load(source)[0]
section["values"] = section["values"][:11]
expression = """(()=>{let r;const queue=window.webpackChunkvrwebui,marker=[['sranipatch-status-probe-'+Date.now()],{},v=>r=v];queue.push(marker);queue.splice(queue.indexOf(marker),1);const R=r(6540),D=r(2551),S=r(3720).yO,M=r(7813),store=r(1333).HR.settings,key='/settings/driver_sranipatch/statusState',had=store.has(key),old=store.get(key),host=document.createElement('div'),results=[],actual=VRHTML.VRSettings.Get('driver_sranipatch','statusState');host.hidden=true;document.body.appendChild(host);try{D.render(R.createElement(S,{active:true,section:{values:[{name:'/settings/driver_sranipatch/status',type:'string',control:'label',label:'Status'}]}}),host);const before=host.querySelector('.SettingsItem').textContent;if(before.includes('Tracking server running'))throw Error('Original bug did not reproduce');const section=SECTION;D.render(R.createElement(S,{active:true,section}),host);for(let i=0;i<10;i++){D.flushSync(()=>M.runInAction(()=>store.set(key,i)));const rows=[...host.querySelectorAll('.Label')].filter(e=>e.textContent.startsWith('Status:'));const expected=section.values[i+1].label;if(rows.length!==1||rows[0].textContent!==expected)throw Error(JSON.stringify({i,expected,actual:rows.map(e=>e.textContent)}));results.push(rows[0].textContent);}return JSON.stringify({original:before,fixed:results,actualState:actual,settingsState:old});}finally{D.unmountComponentAtNode(host);host.remove();M.runInAction(()=>had?store.set(key,old):store.delete(key));}})()""".replace(
    "SECTION", json.dumps(section)
)
try:
    response = call(
        "Runtime.evaluate", {"expression": expression, "returnByValue": True}
    )
    assert "error" not in response, response
    assert "exceptionDetails" not in response["result"], response["result"]
    result = json.loads(response["result"]["result"]["value"])
    if args.expect_running:
        assert result["actualState"] == 2, result
        assert result["settingsState"] == 2, result
    print(json.dumps(result, indent=2))
finally:
    sock.close()
