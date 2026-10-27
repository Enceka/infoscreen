"""Synthetic loopback SSE for browser tests; no modem or audio device access."""
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
import json
import threading

class CallEvents:
    def __init__(self):
        self.condition=threading.Condition();self.payload=None;self.revision=0;self.closed=False
        owner=self
        class Handler(BaseHTTPRequestHandler):
            def log_message(self,*args):pass
            def do_GET(self):
                if self.path!='/events':self.send_error(404);return
                self.send_response(200);self.send_header('Content-Type','text/event-stream')
                self.send_header('Cache-Control','no-cache')
                self.send_header('Access-Control-Allow-Origin','http://127.0.0.1:8088');self.end_headers()
                seen=-1
                try:
                    while True:
                        with owner.condition:
                            if seen==owner.revision and not owner.closed:owner.condition.wait(.5)
                            if owner.closed:return
                            payload=owner.payload if seen!=owner.revision else None
                            seen=owner.revision
                        self.wfile.write(('data: '+payload+'\n\n' if payload else ': keepalive\n\n').encode())
                        self.wfile.flush()
                except (BrokenPipeError,ConnectionResetError):pass
        class Server(ThreadingHTTPServer):
            allow_reuse_address=True
            daemon_threads=True
        self.server=Server(('127.0.0.1',8089),Handler)
        self.thread=threading.Thread(target=self.server.serve_forever,daemon=True);self.thread.start()
    def publish(self,state,wake):
        payload=json.dumps({'calls':state.get('calls',[]),'audio':state,
                            'wake':wake,'error':state.get('error')},sort_keys=True)
        with self.condition:
            if payload!=self.payload:
                self.payload=payload;self.revision+=1;self.condition.notify_all()
    def close(self):
        with self.condition:self.closed=True;self.condition.notify_all()
        self.server.shutdown();self.server.server_close();self.thread.join(2)
