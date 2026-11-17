#!/usr/bin/env python3
"""Prove Codex's native exec_command default shell on Android without a model."""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import queue
import shlex
import subprocess
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parents[4]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', default=str(Path.home()/'Android/Sdk/platform-tools/adb'))
    args = parser.parse_args()
    identity = uuid.uuid4().hex
    remote = '/data/local/tmp/pilon-codex-shell-'+identity
    proof = ROOT/'build/android/codex/proof'/identity
    proof.mkdir(parents=True)
    marker = 'PILON_NATIVE_DEFAULT_SHELL'
    requests = []
    outputs = []
    token = uuid.uuid4().hex
    adb = [args.adb,'-d']

    class Handler(BaseHTTPRequestHandler):
        def log_message(self,*values): pass
        def do_POST(self):
            if self.headers.get('Authorization') != 'Bearer '+token:
                self.send_error(401); return
            length = int(self.headers.get('Content-Length','0'))
            if length <= 0 or length > 2*1024*1024:
                self.send_error(413); return
            body = json.loads(self.rfile.read(length))
            requests.append(body)
            received = [item for item in body.get('input',[]) if item.get('type')=='function_call_output']
            if received:
                outputs.extend(received)
                item = {'type':'message','id':'proof-message','role':'assistant','status':'completed',
                        'content':[{'type':'output_text','text':'Default shell verified.','annotations':[]}]}
            else:
                item = {'type':'function_call','id':'proof-exec','call_id':'proof-exec','status':'completed',
                        'name':'exec_command','arguments':json.dumps({'cmd':'printf '+marker,'max_output_tokens':500})}
            response = {'id':'proof-response-'+str(len(requests)),'status':'completed','output':[item],
                        'usage':{'input_tokens':10,'output_tokens':10,'total_tokens':20}}
            events = [
                {'type':'response.created','response':{'id':response['id'],'status':'in_progress','output':[]}},
                {'type':'response.output_item.added','output_index':0,'item':item},
                {'type':'response.output_item.done','output_index':0,'item':item},
                {'type':'response.completed','response':response}]
            payload = ''.join('event: '+event['type']+'\ndata: '+json.dumps(event)+'\n\n' for event in events).encode()
            self.send_response(200); self.send_header('Content-Type','text/event-stream')
            self.send_header('Content-Length',str(len(payload))); self.end_headers(); self.wfile.write(payload)

    server = ThreadingHTTPServer(('127.0.0.1',0),Handler)
    threading.Thread(target=server.serve_forever,daemon=True).start()
    remote_port = 20000+int(identity[:6],16)%30000
    reverse = None
    child = None
    messages = []
    incoming = queue.Queue()

    def command(*values, timeout=40):
        result = subprocess.run(adb+list(values),capture_output=True,text=True,timeout=timeout)
        if result.returncode: raise RuntimeError(result.stderr)
        return result.stdout.strip()

    def send(value):
        child.stdin.write(json.dumps(value).encode()+b'\n'); child.stdin.flush()

    def wait(predicate,seconds=40):
        deadline = time.monotonic()+seconds
        while time.monotonic()<deadline:
            value = incoming.get(timeout=max(.1,deadline-time.monotonic()))
            messages.append(value)
            if predicate(value): return value
        raise RuntimeError('Codex protocol deadline expired')

    def rpc(number,method,params):
        send({'id':number,'method':method,'params':params})
        result = wait(lambda value:value.get('id')==number)
        assert 'error' not in result,result
        return result['result']

    helper = ROOT/'builds/android/runtime/arm64-v8a/libpilon_codex.so'
    try:
        command('shell','mkdir','-p',remote+'/codex',remote+'/tmp')
        command('push',str(helper),remote+'/codex-helper')
        command('shell','chmod','700',remote+'/codex-helper')
        command('reverse','--no-rebind','tcp:'+str(remote_port),'tcp:'+str(server.server_port))
        reverse = 'tcp:'+str(remote_port)
        environment = ['HOME='+remote,'CODEX_HOME='+remote+'/codex','TMPDIR='+remote+'/tmp','PILON_FIXTURE_TOKEN='+token]
        version = command('shell',shlex.join(['env',*environment,remote+'/codex-helper','--version']))
        assert '0.167.0' in version,version
        configuration = 'model_providers.fixture={name="Pilon shell fixture",base_url="http://127.0.0.1:'+str(remote_port)+'/v1",wire_api="responses",env_key="PILON_FIXTURE_TOKEN",requires_openai_auth=false}'
        invocation = ['env',*environment,remote+'/codex-helper','app-server','--listen','stdio://',
                      '-c','model_provider="fixture"','-c',configuration,'-c','experimental_use_unified_exec_tool=true']
        with (proof/'codex.log').open('wb') as errors:
            script = 'cd '+shlex.quote(remote)+'; echo $$ > '+shlex.quote(remote+'/pid')+'; exec '+shlex.join(invocation)
            child = subprocess.Popen(adb+['shell','-T',script],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=errors)
            def consume():
                for line in child.stdout:
                    if len(line)>4*1024*1024: break
                    incoming.put(json.loads(line))
            threading.Thread(target=consume,daemon=True).start()
            rpc(1,'initialize',{'clientInfo':{'name':'pilon-shell-proof','version':'1.0'},'capabilities':{'experimentalApi':True}})
            send({'method':'initialized'})
            result = rpc(2,'thread/start',{'model':'pilon-fixture','modelProvider':'fixture','cwd':remote,
                'approvalPolicy':'never','sandbox':'danger-full-access','baseInstructions':'Run the requested command, then finish.'})
            thread = result['thread']['id']
            rpc(3,'turn/start',{'threadId':thread,'input':[{'type':'text','text':'Print the shell proof marker.'}]})
            wait(lambda value:value.get('method')=='turn/completed',seconds=60)
        assert outputs,'No native command output returned to the provider'
        assert marker in str(outputs[0].get('output','')),outputs
        assert 'Process exited with code 0' in str(outputs[0].get('output','')),outputs
        assert requests and len(requests)<=3,len(requests)
        result = {'version':version,'sha256':hashlib.sha256(helper.read_bytes()).hexdigest(),
                  'shellOmitted':True,'nativeOutput':outputs,'providerRequests':len(requests),
                  'scope':'isolated adb-shell native execution; app-UID/UI approval acceptance is separate'}
        (proof/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        print(proof/'result.json')
    finally:
        (proof/'protocol.json').write_text(json.dumps(messages,indent=2)+'\n')
        if child is not None and child.poll() is None:
            child.stdin.close()
            try:child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pid=command('shell','cat',remote+'/pid')
                if pid.isdigit() and command('shell','readlink','/proc/'+pid+'/exe')==remote+'/codex-helper':
                    command('shell','kill','-TERM',pid)
                child.terminate();child.wait(timeout=5)
        if reverse is not None:command('reverse','--remove',reverse)
        command('shell','rm','-rf',remote)
        server.shutdown();server.server_close()


if __name__=='__main__': main()
