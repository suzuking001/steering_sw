// Offline browser integration test. The HTTP fixture uses the production C++
// JSON codec; ESP32 networking, NVS and the real BLE transport still need hardware.
import http from 'node:http';
import {readFile,writeFile,mkdir,mkdtemp,rm} from 'node:fs/promises';
import {spawn,execFileSync} from 'node:child_process';
import {tmpdir} from 'node:os';
import {resolve,join} from 'node:path';
import {once} from 'node:events';
import assert from 'node:assert/strict';
const root=resolve(import.meta.dirname,'..'), binary=join(root,'.pio/native-tests/test_main');
const fixture=()=>JSON.parse(execFileSync(binary,['--fixture'],{encoding:'utf8'}));
let fixtureCsrf='fixture-csrf',inputModePaused=false,delayInputMode=false,delayInputResume=false;
let pausedTestRequests=0;
const inputModeRequests=[];
let wifiState='unconfigured',wifiError='',wifiPasswordSet=false,wifiAttempts=0,rejectNextPin=false;
const savedWifiRequests=[],saveOrder=[];
let state=fixture(),learning=null,learningAt=0,passcodeSet=false,testedActions=[];
let macroBusy=false, stopRequests=0;
let bleAttached=true, bleEncrypted=true, bleReconnectRequests=0;
let bleKeyboard=true, bleMedia=true, bleProtocol='report', bleMapRead=false, bleDisconnectReason=0;
let key2Millivolts=2750;
const firmware={repository:'suzuking001/steering_sw',version:'1.0.1',availableVersion:'',stage:'idle',tokenSet:false,received:0,size:0,message:'',bootChecking:false};
const firmwareRequests=[];
const statusRequests=[];
const errors=[];let checks=0;
function check(condition,message){assert.ok(condition,message);checks++;console.log('PASS '+message);}
const csp="default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' blob:; base-uri 'none'; frame-ancestors 'none'";
const server=http.createServer(async(req,res)=>{
  const reply=(code,value)=>{res.writeHead(code,{'Content-Type':'application/json; charset=utf-8','Cache-Control':'no-store'});res.end(JSON.stringify(value));};
  try {
    if(!req.url.startsWith('/api/')){
      const file={'/':'index.html','/app.js':'app.js','/style.css':'style.css'}[req.url];
      if(!file){res.writeHead(404);res.end();return;}
      res.writeHead(200,{'Content-Type':file.endsWith('.js')?'text/javascript':file.endsWith('.css')?'text/css':'text/html; charset=utf-8','Content-Security-Policy':csp});res.end(await readFile(join(root,'web',file)));return;
    }
    let raw='';for await(const part of req)raw+=part;
    const body=raw?JSON.parse(raw):{};
    const requestURL=new URL(req.url,'http://localhost');
    const path=requestURL.pathname.slice(5);
    if(req.method!=='GET'&&req.headers['x-csrf-token']!==fixtureCsrf){reply(403,{error:'CSRF'});return;}
    if(path==='session'){reply(200,{csrf:fixtureCsrf});return;}
    if(path==='status'){
      const includeLogs=requestURL.searchParams.get('logs')!=='0';
      statusRequests.push(includeLogs);
      if(['checking','downloading','verifying','restarting'].includes(firmware.stage)){
        reply(200,{firmware,wifiConnected:wifiState==='connected'});return;
      }
      const done=learning&&Date.now()-learningAt>400;
      const paused=!state.ble.enabled, connected=bleAttached&&!paused;
      const encrypted=connected&&bleEncrypted;
      const ble={...state.ble,advertising:!paused&&!connected,encrypted,keyboardReady:encrypted&&bleKeyboard,mediaReady:encrypted&&bleMedia,
        protocolMode:bleProtocol,reportMapRead:bleMapRead,disconnectReason:bleDisconnectReason,bonds:1,
        advertisingIntervalMs:!paused&&!connected?20:undefined,automaticReconnects:1,serviceRefreshes:1,automaticRetryUsed:true};
      reply(200,{uptimeMs:Date.now()%100000,samples:12345,outputEnabled:state.outputEnabled,bleConnected:connected,bleReady:ble.keyboardReady||ble.mediaReady,bleSuspended:paused,inputModePaused,ble,busy:macroBusy,passcodeSet,accMv:2000,
        readings:state.channels.map((c,i)=>({name:c.name,mode:c.mode,pin:[36,39][i],raw:3890,adc:3890,mv:i===0?3050:key2Millivolts,ohm:i===0?5211:5187,button:'未押下',buttonId:null})),
        firmware,learning:{active:!!learning&&!done,done:!!done,channel:learning?.channel||0,count:400,error:'',remainingMs:done?0:200,center:3895,minimum:3890,maximum:3900,tolerance:15},
        wifiConfigured:!!state.ssid,wifiSavedSsid:state.ssid,wifiPasswordSet,wifiState,wifiError,wifiAttempts,wifiConnectedSsid:wifiState==='connected'?state.ssid:'',wifiConnected:wifiState==='connected',ip:wifiState==='connected'?'192.168.0.45':'0.0.0.0',wifiLanUrl:wifiState==='connected'?'http://192.168.0.45/':'',mdnsReady:wifiState==='connected',rssi:-45,apActive:true,apName:'SteeringSW-test',apIp:'192.168.4.1',freeHeap:140000,logs:includeLogs?[{at:100,message:'Webテスト：実機のBLE・NVSは未検証'}]:undefined});return;
    }
    if(path==='config'&&req.method==='GET'||path==='backup'){reply(200,state);return;}
    if(path==='defaults'){const data=fixture();data.ssid=state.ssid;reply(200,data);return;}
    if(['config','validate','restore'].includes(path)){
      try{
        const parsed=JSON.parse(execFileSync(binary,['--validate'],{input:JSON.stringify(body),encoding:'utf8',stdio:['pipe','pipe','pipe']}));
        if(path==='validate')reply(200,parsed);else{state=parsed;learning=null;reply(200,{message:'設定を保存・適用しました'});}return;
      }catch(error){reply(400,JSON.parse(error.stdout||'{"error":"validation failed"}'));return;}
    }
    if(path==='learn/start'){learning={channel:body.channel};learningAt=Date.now();reply(200,{message:'学習開始'});return;}
    if(path==='learn/stop'){learning=null;reply(200,{message:'学習中止'});return;}
    if(path==='test'){
      if(body.action==='passcode'){reply(409,{error:'テストできない操作です'});return;}
      if(inputModePaused){pausedTestRequests++;reply(409,{error:'キー送信が一時停止中です'});return;}
      testedActions.push(body.action);reply(200,{message:'テストを開始しました'});return;
    }
    if(path==='firmware/check'){
      if(wifiState!=='connected'){reply(409,{error:'更新できません'});return;}
      firmwareRequests.push(path);firmware.stage='checking';reply(202,{message:'更新確認を開始'});return;
    }
    if(path==='firmware/install'){
      if(firmware.stage!=='available'||body.version!==firmware.availableVersion){reply(409,{error:'更新を確認し直してください'});return;}
      firmwareRequests.push(body.version);firmware.stage='downloading';reply(202,{message:'更新を開始'});return;
    }
    if(path==='ble/input-mode'){
      inputModeRequests.push(body);
      if(body.enabled?delayInputMode:delayInputResume)await new Promise(resolve=>setTimeout(resolve,400));
      inputModePaused=body.enabled;
      if(inputModePaused)macroBusy=false;
      reply(200,{message:'文字入力の状態を更新しました'});return;
    }
    if(path==='ble/reconnect'){
      if(!state.ble.enabled){reply(409,{error:'Bluetoothを有効にしてください'});return;}
      bleReconnectRequests++;bleAttached=false;reply(200,{message:'Bluetoothの再接続を待っています'});return;
    }
    if(path==='passcode'){if(rejectNextPin){rejectNextPin=false;reply(507,{error:'保存できませんでした'});return;}saveOrder.push('passcode');passcodeSet=!!body.passcode;reply(200,{message:'パスコード更新'});return;}
    if(path==='wifi'){if(body.ssid&&body.ssid!==state.ssid&&!Object.hasOwn(body,'password')){reply(400,{error:'新しいWi-Fiのパスワードを入力してください'});return;}savedWifiRequests.push(body);saveOrder.push('wifi');state.ssid=body.ssid;if(Object.hasOwn(body,'password'))wifiPasswordSet=!!body.password;wifiState=state.ssid?'connecting':'unconfigured';wifiAttempts=1;reply(200,{message:'Wi-Fi設定保存'});return;}
    if(path==='wifi/reconnect'){wifiState='connecting';wifiAttempts++;reply(200,{message:'再接続を開始しました'});return;}
    if(path==='reset'){state=fixture();passcodeSet=false;reply(200,{message:'初期化しました'});return;}
    if(path==='stop'){stopRequests++;macroBusy=false;reply(200,{message:'送信を停止しました'});return;}
    if(['reboot','ble/forget'].includes(path)){reply(200,{message:'完了'});return;}
    reply(404,{error:'unknown endpoint'});
  }catch(error){errors.push(error.stack);reply(500,{error:String(error)});}
});
server.listen(0,'127.0.0.1');await once(server,'listening');
const url='http://127.0.0.1:'+server.address().port;
const output=join(root,'.pio/ui-tests');await mkdir(output,{recursive:true});
const profile=await mkdtemp(join(tmpdir(),'steering-browser-'));
let chrome,ws;
try{
  chrome=spawn(process.env.CHROMIUM||'chromium',['--headless','--no-sandbox','--disable-dev-shm-usage','--disable-gpu','--remote-debugging-port=0','--user-data-dir='+profile,'about:blank'],{stdio:['ignore','ignore','pipe']});
  let browserURL='';
  await new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(new Error('Chromium startup timed out')),15000);chrome.on('error',reject);chrome.stderr.on('data',data=>{const match=data.toString().match(/DevTools listening on (ws:\/\/[^\s]+)/);if(match){browserURL=match[1];clearTimeout(timer);resolve();}});});
  const debugOrigin=browserURL.replace(/^ws:/,'http:').split('/devtools/')[0];
  const pages=await(await fetch(debugOrigin+'/json/list')).json();
  ws=new WebSocket(pages.find(p=>p.type==='page').webSocketDebuggerUrl);await once(ws,'open');
  let sequence=0;const pending=new Map();
  ws.addEventListener('message',event=>{const msg=JSON.parse(event.data);if(msg.id){const p=pending.get(msg.id);pending.delete(msg.id);if(msg.error)p.reject(new Error(JSON.stringify(msg.error)));else p.resolve(msg.result);}else if(msg.method==='Runtime.exceptionThrown')errors.push(msg.params.exceptionDetails.exception?.description||msg.params.exceptionDetails.text);else if(msg.method==='Page.javascriptDialogOpening')call('Page.handleJavaScriptDialog',{accept:true});});
  function call(method,params={}){return new Promise((resolve,reject)=>{const id=++sequence;pending.set(id,{resolve,reject});ws.send(JSON.stringify({id,method,params}));});}
  async function evaluate(expression){const out=await call('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true});if(out.exceptionDetails)throw new Error(out.exceptionDetails.exception?.description||out.exceptionDetails.text);return out.result?.value;}
  async function wait(expression){for(let i=0;i<150;i++){if(await evaluate(expression))return;await new Promise(r=>setTimeout(r,100));}throw new Error('Timed out: '+expression);}
  await call('Page.enable');await call('Runtime.enable');
  await call('Emulation.setDeviceMetricsOverride',{width:1280,height:1000,deviceScaleFactor:1,mobile:false});
  await call('Page.navigate',{url});await wait('!document.getElementById("workspace").hidden && currentStatus !== undefined');
  await evaluate(`globalThis.fill=(root,label,value)=>{const node=[...document.querySelector(root).querySelectorAll('label')].find(x=>x.firstChild?.textContent===label)?.querySelector('input');if(!node)throw new Error('Field not found: '+label);node.value=value;node.dispatchEvent(new Event('input',{bubbles:true}));};`);
  check(await evaluate('!document.getElementById("login-form") && !document.getElementById("admin-form") && !document.getElementById("logout")'),'local network opens settings without Web login or password controls');
  check(await evaluate('document.querySelectorAll(".button-card").length === 9 && document.querySelectorAll(".macro-card").length === 5'),'nine buttons and five editable shortcuts render');
  check(await evaluate('activeTab==="buttons" && document.querySelectorAll("nav [data-tab]").length===3'),'opens directly to button assignments with three tabs');
  const idleStart=statusRequests.length;
  await new Promise(resolve=>setTimeout(resolve,1300));
  const idleRequests=statusRequests.slice(idleStart);
  check(idleRequests.length>=1 && idleRequests.length<=2 && idleRequests.every(request=>!request),
    'ordinary screens poll about once a second and do not request log data');
  await evaluate(`Object.defineProperty(document,'hidden',{value:true,configurable:true});
    document.dispatchEvent(new Event('visibilitychange'));`);
  const hiddenStart=statusRequests.length;
  await new Promise(resolve=>setTimeout(resolve,1100));
  check(statusRequests.length===hiddenStart,'a background page makes no new status requests');
  await evaluate(`delete document.hidden;document.dispatchEvent(new Event('visibilitychange'));`);
  const stopStartsHidden=await evaluate('document.getElementById("stop").hidden');
  macroBusy=true;
  await wait('!document.getElementById("stop").hidden');
  await evaluate(`document.getElementById('stop').click()`);
  await wait('document.getElementById("stop").hidden');
  check(stopStartsHidden && stopRequests===1 && !macroBusy,'the stop button appears only during a send and cancels it');
  await evaluate(`document.querySelector('[data-tab="settings"]').click()`);
  await wait('document.getElementById("metric-0-0").textContent==="3,890" && liveMonitorVisible && document.getElementById("chart-0").width>300');
  check(await evaluate('document.getElementById("metric-0-0").textContent==="3,890" && [...document.querySelectorAll("#live-cards canvas")].every(chart=>chart.width>300)'),'two live charts are drawn in settings');
  await evaluate(`document.getElementById('operation-log').open=true;`);
  await wait('document.querySelector("#logs li")');
  const liveStart=statusRequests.length;
  await evaluate(`globalThis.logRow=document.querySelector('#logs li');
    globalThis.canvasChanges=0;
    const canvasObserver=new MutationObserver(records=>{canvasChanges+=records.length;});
    for (const chart of document.querySelectorAll('#live-cards canvas')) canvasObserver.observe(chart,{attributes:true,attributeFilter:['width','height']});`);
  await new Promise(resolve=>setTimeout(resolve,750));
  check(statusRequests.slice(liveStart).filter(Boolean).length>=2 &&
    await evaluate('document.querySelector("#logs li")===logRow && canvasChanges===0'),
    'live monitoring remains fast without recreating unchanged logs or resizing canvas buffers');
  await evaluate(`document.getElementById('operation-log').open=false;`);
  bleEncrypted=false;
  await wait('document.getElementById("ble-state").textContent==="暗号化待ち"');
  check(await evaluate(`document.getElementById('ble-encryption').textContent==='準備中' &&
    document.getElementById('ble-advertising').textContent==='接続済み' &&
    document.getElementById('ble-keyboard').textContent==='準備中' && document.getElementById('ble-paired').textContent==='1台'`),
    'Bluetooth panel distinguishes the link, security, key services and saved pairings');
  bleEncrypted=bleMapRead=true;
  bleKeyboard=bleMedia=false;
  bleDisconnectReason=0x213;
  await wait('document.getElementById("ble-state").textContent==="キー通知待ち"');
  check(await evaluate(`document.getElementById('ble-encryption').textContent==='完了' &&
    document.getElementById('ble-diagnostic').textContent.includes('読み取りあり') &&
    document.getElementById('ble-diagnostic').textContent.includes('0x213') &&
    document.getElementById('ble-diagnostic').textContent.includes('サービス情報の再確認を通知 1回') &&
    document.getElementById('ble-diagnostic').textContent.includes('自動再試行済み')`), 'encrypted links show service refresh and the end of automatic recovery');
  bleProtocol='boot';
  bleKeyboard=true;
  await wait('document.getElementById("ble-state").textContent==="一部操作可能"');
  check(await evaluate(`document.getElementById('ble-keyboard').textContent==='使用可能' &&
    document.getElementById('ble-media').textContent==='互換モードでは使用不可'`), 'boot protocol can expose keyboard-only operation');
  bleProtocol='report';
  bleMedia=true;
  await evaluate(`document.getElementById('reconnect-ble').click()`);
  await wait('document.getElementById("ble-advertising").textContent==="受付中"');
  check(bleReconnectRequests===1 && await evaluate(`document.getElementById('ble-paired').textContent==='1台' &&
    document.getElementById('ble-diagnostic').textContent.includes('20 ms間隔') &&
    document.getElementById('ble-diagnostic').textContent.includes('自動再接続の試行 1回')`),
    'Bluetooth reconnect retains pairings and displays the advertising interval and retry count');
  await evaluate(`fill('#ble-fields','端末に表示されるBluetooth名','運転席 Steering');document.getElementById('ble-enabled-input').click();
    document.getElementById('save').click();`);
  await wait('!dirty && document.getElementById("ble-state").textContent==="無効"');
  check(state.ble.name==='運転席 Steering' && !state.ble.enabled && await evaluate('document.getElementById("reconnect-ble").disabled'),
    'Bluetooth name and enable setting save through the normal footer');
  bleAttached=bleEncrypted=true;
  await evaluate(`fill('#ble-fields','端末に表示されるBluetooth名','Steering Switch');document.getElementById('ble-enabled-input').click();
    document.getElementById('save').click();`);
  await wait('!dirty && document.getElementById("ble-state").textContent==="操作可能"');
  let shot=await call('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});await writeFile(join(output,'desktop.png'),Buffer.from(shot.data,'base64'));
  await evaluate(`document.querySelector('#input-cards').closest('details').open=true;fill('#input-cards','供給電圧 mV',3200);`);
  check(await evaluate('dirty && config.channels[0].supplyMv===3200'),'circuit constants are editable without changing saved state');
  await evaluate(`document.getElementById('save').click()`);await wait('!dirty');check(state.channels[0].supplyMv===3200,'saving applies validated circuit constants');
  const referenceCenter=state.buttons[0].center, overlappingCenter=state.buttons[1].center;
  await evaluate(`document.querySelector('[data-tab="buttons"]').click();document.querySelector('#button-cards details').open=true;fill('#button-cards','中心値',${overlappingCenter});document.getElementById('save').click();`);
  await wait('document.getElementById("message").textContent.includes("重複")');
  check(state.buttons[0].center===referenceCenter,'overlapping ranges are rejected without overwriting saved config');
  await evaluate(`fill('#button-cards','中心値',${referenceCenter});document.getElementById('save').click();`);await wait('!dirty');
  await evaluate(`document.getElementById('add-button').click();`);
  check(await evaluate('document.getElementById("button-name-dialog").open && document.getElementById("button-name-input").value==="ボタン1"'),'adding a button opens a dialog with a unique default name');
  await evaluate(`document.getElementById('button-name-form').requestSubmit();document.getElementById('add-button').click();`);
  check(await evaluate('document.querySelectorAll(".button-card").length===10 && document.getElementById("button-name-input").value==="ボタン2"'),'submitting adds a card and advances the default name');
  await evaluate(`document.getElementById('cancel-button-name').click();[...document.querySelectorAll('.learn-button')].at(-1).click();`);
  await wait('document.getElementById("learn-dialog").open && !document.getElementById("capture-1").disabled');
  check(await evaluate('currentStatus.inputModePaused && currentStatus.bleConnected && document.getElementById("learn-mv-0").textContent==="3050 mV" && document.getElementById("learn-mv-1").textContent==="2750 mV"'),'learning displays both measured voltages with key output paused and Bluetooth connected');
  delayInputResume=true;
  const beforeLearningTest=testedActions.length;
  await evaluate(`document.getElementById('capture-1').click();
    const volumeId=config.buttons.find(button=>button.shortAction==='volume_up').id;
    const test=document.querySelector('[data-button-id="'+volumeId+'"] button[aria-label="短押しをテスト"]');
    test.click();test.click();`);
  await wait('!document.getElementById("learn-dialog").open && !currentStatus.inputModePaused && !testPending');
  check(pausedTestRequests===0 && testedActions.slice(beforeLearningTest).join(',')==='volume_up',
    'testing immediately after learning waits for the resume response and queues no duplicate command');
  delayInputResume=false;
  check(await evaluate('config.buttons[9].channel===1 && config.buttons[9].center===2750 && dirty'),'KEY2 capture assigns the channel and voltage to the draft card');
  check(state.buttons.length===9,'learning does not silently save the card');
  await evaluate(`config.buttons[9].name='<img src=x onerror=alert(1)>';renderButtons();`);
  check(await evaluate('document.querySelectorAll("#button-cards img").length===0 && document.getElementById("button-cards").textContent.includes("<img")'),'user-defined labels render as text');
  await evaluate(`globalThis.newCard='[data-button-id="'+config.buttons[9].id+'"]';
    globalThis.testButtons=()=>[...document.querySelectorAll(newCard+' .button-actions button')];
    globalThis.setAction=(index,value)=>{
      const select=document.querySelectorAll(newCard+' .button-actions select')[index];
      select.value=value;select.dispatchEvent(new Event('change',{bubbles:true}));
    };`);
  check(await evaluate('testButtons().length===3 && testButtons().every(button=>button.disabled)'),
    'unassigned actions cannot be tested from cards');
  await evaluate(`document.getElementById('message').hidden=true;
    document.querySelector('[data-button-id="phone_on"] button[aria-label="数字入力をテスト"]').click();`);
  await wait('!document.getElementById("message").hidden && document.getElementById("message").textContent.includes("テストを開始")');
  check(testedActions.at(-1)==='test_digits','the passcode card requests the sample digits action');
  const cardActions=['play_pause','next','previous'];
  for (const [index, action] of cardActions.entries()) {
    await evaluate(`setAction(${index},${JSON.stringify(action)});document.getElementById('message').hidden=true;testButtons()[${index}].click();`);
    await wait('!document.getElementById("message").hidden && document.getElementById("message").textContent.includes("テストを開始")');
  }
  check(testedActions.slice(-3).join(',')===cardActions.join(',') && state.buttons.length===9,
    'each test button previews its selected action without saving the assignment');
  await evaluate(`config.macros.find(macro=>macro.id==='maps').steps[0].key=35;
    setAction(2,'maps');testButtons()[2].click();`);
  await wait('document.getElementById("message").textContent.includes("マクロの変更を保存")');
  check(testedActions.at(-1)==='previous','a card does not test a stale macro definition');
  await evaluate(`config.macros.find(macro=>macro.id==='maps').steps=clone(saved.macros.find(macro=>macro.id==='maps').steps);setAction(2,'previous');`);
  await evaluate(`
    document.querySelector(newCard+' details').open=true;
    fill(newCard,'ボタン名','追加したボタン');fill(newCard,'中心値',180);fill(newCard,'許容幅 ±',12);
    document.getElementById('save').click();
  `);
  await wait('!dirty');
  check(state.buttons[9].name==='追加したボタン' && state.buttons[9].center===180 && state.buttons[9].tolerance===12 && state.buttons[9].shortAction==='play_pause',
    'the captured card can be edited and saved with its action');
  await evaluate(`config.buttons.pop();renderButtons();document.getElementById('save').click();`);await wait('!dirty');
  key2Millivolts=3100;
  await wait('currentStatus.readings[1].mv===3100');
  const pausesBeforeIdleCapture=inputModeRequests.length;
  check(await evaluate(`document.querySelector('[data-tab="settings"]').click();
    document.querySelector('#input-cards').closest('details').open=true;
    globalThis.idleFields=[...document.querySelectorAll('#input-cards .field-with-action')];
    lastReadingAt=performance.now()-1500;
    idleFields[0].querySelector('button').click();
    !dirty && document.getElementById('message').textContent.includes('最新の測定値がありません')`),
    'idle capture rejects stale readings without changing the draft');
  await wait('hasFreshReading()');
  check(await evaluate(`idleFields.forEach(row=>row.querySelector('button').click());
    dirty && !document.getElementById('learn-dialog').open &&
      idleFields.every(row=>row.querySelector('button').textContent==='現在値を設定する') &&
      config.channels[0].idleCenter===3050 && config.channels[1].idleCenter===3100 &&
      idleFields.map(row=>row.querySelector('input').value).join(',')==='3050,3100'`),
    'both idle capture buttons update their fields immediately without a modal');
  await evaluate(`document.getElementById('save').click();`);await wait('!dirty');
  check(inputModeRequests.length===pausesBeforeIdleCapture && state.channels[0].idleCenter===3050 && state.channels[1].idleCenter===3100,
    'idle values can be saved without pausing Bluetooth');
  await evaluate(`document.querySelector('[data-tab="macros"]').click();`);
  check(await evaluate(`globalThis.macroShortcut=()=>[...document.querySelectorAll('.macro-shortcut')].map(label=>label.textContent);
    const initial=macroShortcut();
    fill('#macro-cards .macro-card:nth-child(2)','HIDキー番号（10進数）',30);
    [...document.querySelectorAll('#macro-cards .macro-card:nth-child(2) .modifiers input')].slice(0,2).forEach(input=>input.click());
    initial[0]==='送信するキー Control＋Option＋Shift＋1' && initial[1]==='送信するキー Control＋Option＋Shift＋2' &&
    macroShortcut()[1]==='送信するキー Option＋1'`), 'preset chords reflect the actual key and modifiers');
  await evaluate(`document.getElementById('discard').click();document.getElementById('add-macro').click();`);
  check(await evaluate('config.macros.length===6 && document.querySelectorAll(".macro-card").length===6'),'custom macros can be added');
  await evaluate(`document.getElementById('save').click()`);await wait('!dirty');
  await evaluate(`document.querySelectorAll('#macro-cards .macro-card')[0].querySelector('.actions button:last-child').click()`);
  await wait('document.getElementById("message").textContent.includes("テストを開始")');check(testedActions.includes('macro:maps'),'macro test submits the saved macro action');
  await evaluate(`document.querySelector('[data-tab="settings"]').click();document.getElementById('passcode').value='4826';document.getElementById('passcode-form').requestSubmit();`);
  await wait('currentStatus.passcodeSet===true');check(await evaluate('document.getElementById("passcode").value==="4826" && document.getElementById("passcode-feedback").textContent.includes("保存")'),'passcode field remains populated with a saved indicator');
  check(await evaluate(`api('backup').then(data=>!('passcode' in data)&&!('wifiPassword' in data)&&!('adminPassword' in data))`),'backup contains no secret fields');
  const imported=fixture();imported.name='復元テスト';imported.ssid='other-network';imported.outputEnabled=true;
  const importFile=join(profile,'restore.json');await writeFile(importFile,JSON.stringify(imported));
  const doc=await call('DOM.getDocument');const input=await call('DOM.querySelector',{nodeId:doc.root.nodeId,selector:'#restore-file'});
  await call('DOM.setFileInputFiles',{nodeId:input.nodeId,files:[importFile]});
  await wait('config.name==="復元テスト"');
  check(await evaluate('dirty && config.outputEnabled===false && config.ssid===saved.ssid'),'restore stages validated config, disables output and preserves network');
  const invalid=cloneJSON(imported);invalid.channels=[null,null];await writeFile(importFile,JSON.stringify(invalid));
  await call('DOM.setFileInputFiles',{nodeId:input.nodeId,files:[importFile]});
  await wait('document.getElementById("message").textContent.includes("不正")');
  check(await evaluate('config.name==="復元テスト" && config.channels[0]!==null'),'malformed import leaves the current draft intact');
  await evaluate(`document.getElementById('discard').click();document.querySelector('[data-tab="buttons"]').click()`);
  await call('Emulation.setDeviceMetricsOverride',{width:390,height:844,deviceScaleFactor:1,mobile:true});
  await new Promise(r=>setTimeout(r,300));
  check(await evaluate('document.documentElement.scrollWidth <= innerWidth'),'mobile viewport has no horizontal page overflow');
  shot=await call('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});await writeFile(join(output,'mobile.png'),Buffer.from(shot.data,'base64'));
  fixtureCsrf='next-boot-csrf';
  await call('Network.clearBrowserCookies');await call('Page.reload');
  await wait('typeof csrf !== "undefined" && csrf==="next-boot-csrf" && !document.getElementById("workspace").hidden && currentStatus !== undefined');
  check(await evaluate('csrf==="next-boot-csrf" && !document.getElementById("login-form")'),'fresh browser retrieves a new device token without a password or session cookie');
  check(await evaluate(`fetch('/api/stop',{method:'POST'}).then(r=>r.status===403)`),'local access still rejects writes without CSRF');
  await evaluate(`document.querySelector('[data-tab="settings"]').click();globalThis.editingInput=document.querySelector('#general-fields input[type=text]');editingInput.focus();editingInput.value='端末で入力中';editingInput.dispatchEvent(new Event('input',{bubbles:true}));`);
  await new Promise(r=>setTimeout(r,650));
  check(await evaluate('document.activeElement===editingInput && editingInput.value==="端末で入力中"'),'telemetry polling preserves text field focus and draft input');
  await evaluate(`document.getElementById('save').click()`);await wait('!dirty');
  check(state.name==='端末で入力中','local network can save configuration without Web login');
  await evaluate(`
    document.getElementById('wifi-ssid').value='TestHome24';document.getElementById('wifi-ssid').dispatchEvent(new Event('input',{bubbles:true}));
    document.getElementById('wifi-password').value='test-pass-123';document.getElementById('wifi-password').dispatchEvent(new Event('input',{bubbles:true}));
    document.getElementById('passcode').value='5931';document.getElementById('passcode').dispatchEvent(new Event('input',{bubbles:true}));document.getElementById('passcode').focus();
  `);
  await new Promise(r=>setTimeout(r,650));
  check(await evaluate('document.getElementById("wifi-password").value==="test-pass-123" && document.getElementById("passcode").value==="5931" && document.activeElement===document.getElementById("passcode")'),'Wi-Fi and passcode inputs remain populated and focused during polling');
  check(await evaluate('wifiDirty && passcodeDirty && !document.getElementById("save").disabled'),'footer save includes Wi-Fi and passcode edits');
  await evaluate(`document.getElementById('show-wifi-password').click();document.getElementById('show-passcode').click();`);
  check(await evaluate('document.getElementById("wifi-password").type==="text" && document.getElementById("passcode").type==="text"'),'entered Wi-Fi password and passcode can be revealed');
  await evaluate(`globalThis.generalInput=document.querySelector('#general-fields input[type=text]');generalInput.value='接続設定中';generalInput.dispatchEvent(new Event('input',{bubbles:true}));document.getElementById('save').click()`);await wait('!hasChanges() && currentStatus.wifiSavedSsid==="TestHome24"');
  check(state.ssid==='TestHome24'&&saveOrder.slice(-2).join(',')==='passcode,wifi','footer saves passcode before changing the Wi-Fi connection');
  check(await evaluate('document.getElementById("wifi-password").value==="test-pass-123" && document.getElementById("passcode").value==="5931"'),'saving does not clear either secret input');
  check(await evaluate('document.getElementById("wifi-saved-state").textContent.includes("TestHome24") && document.getElementById("wifi-connection-state").textContent.includes("接続中")'),'Wi-Fi saved state is distinct from connection progress');
  wifiState='failed';wifiError='認証に失敗しました。パスワードを確認してください。';
  await wait('document.getElementById("wifi-connection-state").textContent.includes("認証に失敗")');
  check(await evaluate('document.getElementById("wifi-saved-state").textContent.includes("保存済み") && document.getElementById("wifi-lan-access").hidden'),'failed connection still shows saved configuration and hides invalid LAN URL');
  await evaluate(`document.getElementById('wifi-reconnect').click()`);await wait('currentStatus.wifiState==="connecting"');check(wifiAttempts===2,'saved Wi-Fi settings can be retried explicitly');
  wifiState='connected';wifiError='';await wait('!document.getElementById("wifi-lan-access").hidden');
  check(await evaluate('document.getElementById("wifi-lan-url").textContent==="http://192.168.0.45/" && document.getElementById("wifi-lan-detail").textContent.includes("TestHome24")'),'successful connection displays the router-side PC URL and network name');
  await evaluate(`document.getElementById('wifi-panel').scrollIntoView()`);
  shot=await call('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});await writeFile(join(output,'wifi-connected.png'),Buffer.from(shot.data,'base64'));
  await call('Page.reload');await wait('!document.getElementById("workspace").hidden && currentStatus?.wifiSavedSsid==="TestHome24" && document.getElementById("wifi-password").value===""');
  check(await evaluate('document.getElementById("wifi-password").placeholder.includes("登録済み") && document.getElementById("passcode").placeholder.includes("登録済み") && document.getElementById("passcode").value===""'),'reload shows registered placeholders without reading saved secrets back');
  await evaluate(`document.querySelector('[data-tab="settings"]').click();document.getElementById('wifi-ssid').value='DifferentNetwork';document.getElementById('wifi-ssid').dispatchEvent(new Event('input',{bubbles:true}));document.getElementById('wifi-form').requestSubmit();`);
  await wait('document.getElementById("wifi-feedback").textContent.includes("新しいWi-Fi")');
  check(await evaluate('document.getElementById("wifi-ssid").value==="DifferentNetwork" && wifiDirty'),'failed Wi-Fi save retains the draft and displays a persistent error');
  await evaluate(`document.getElementById('wifi-ssid').value='TestHome24';document.getElementById('wifi-ssid').dispatchEvent(new Event('input',{bubbles:true}));document.getElementById('wifi-form').requestSubmit();`);await wait('!wifiDirty');
  check(!Object.hasOwn(savedWifiRequests.at(-1),'password')&&wifiPasswordSet,'empty password for the saved SSID preserves the stored credential');
  rejectNextPin=true;
  await evaluate(`document.getElementById('passcode').value='6042';document.getElementById('passcode').dispatchEvent(new Event('input',{bubbles:true}));document.getElementById('passcode-form').requestSubmit();`);
  await wait('document.getElementById("passcode-feedback").textContent.includes("保存できませんでした")');
  check(await evaluate('document.getElementById("passcode").value==="6042" && passcodeDirty'),'passcode storage failure retains the entered value and unsaved state');
  fixtureCsrf='restarted-token';const priorPinSaves=saveOrder.filter(x=>x==='passcode').length;
  await evaluate(`document.getElementById('passcode-form').requestSubmit()`);await wait('!passcodeDirty');
  check(saveOrder.filter(x=>x==='passcode').length===priorPinSaves+1&&await evaluate('csrf==="restarted-token" && document.getElementById("passcode").value==="6042"'),'device token refresh retries a rejected save once without losing input');
  const beforeDesktopFocus=inputModeRequests.length;
  await evaluate(`document.getElementById('wifi-password').focus();document.getElementById('passcode').focus()`);
  check(inputModeRequests.length===beforeDesktopFocus,'desktop password focus does not pause key output');
  await call('Page.addScriptToEvaluateOnNewDocument',{source:`
    Object.defineProperty(navigator,'platform',{value:'MacIntel'});
    Object.defineProperty(navigator,'maxTouchPoints',{value:5});
  `});
  await call('Page.reload');
  await wait('typeof isTouchEditor !== "undefined" && isTouchEditor && typeof currentStatus !== "undefined" && currentStatus.bleConnected');
  check(await evaluate('!document.querySelector("#input-mode, [data-input-mode]")'),'the input mode buttons are removed');
  delayInputMode=true;delayInputResume=true;
  const beforeTouchTest=testedActions.length, beforeTouchPause=inputModeRequests.length;
  await evaluate(`const volumeId=config.buttons.find(button=>button.shortAction==='volume_up').id;
    const card=document.querySelector('[data-button-id="'+volumeId+'"]');
    card.querySelector('input[type="text"]').focus();
    card.querySelector('button[aria-label="短押しをテスト"]').click();`);
  await wait('!inputModeRequest && !testPending');
  check(await evaluate('!activeInputField() && document.getElementById("message").textContent.includes("テストを開始")') &&
    pausedTestRequests===0 && testedActions.slice(beforeTouchTest).join(',')==='volume_up' &&
    inputModeRequests.slice(beforeTouchPause).map(request=>request.enabled).join(',')==='true,false',
    'one touchscreen test click closes a focused field and waits for its in-flight pause and resume');
  delayInputMode=false;delayInputResume=false;
  await evaluate(`document.querySelector('[data-tab="settings"]').click();document.getElementById('wifi-password').focus()`);
  await wait('currentStatus.inputModePaused && !inputModeRequest');
  check(await evaluate(`document.activeElement.id==='wifi-password' && currentStatus.bleReady &&
    currentStatus.bleConnected && !currentStatus.bleSuspended && document.getElementById('input-mode-help').textContent.includes('キー送信')`),
    'touchscreen password focus pauses key output while the Bluetooth link and field stay active');
  await wait('!document.getElementById("field-keyboard").hidden');
  await evaluate(`Object.defineProperty(visualViewport,'height',{value:document.documentElement.clientHeight-250,configurable:true});
    visualViewport.dispatchEvent(new Event('resize'));`);
  check(await evaluate(`document.getElementById('field-keyboard').hidden && document.activeElement.id==='wifi-password' &&
    currentStatus.inputModePaused && currentStatus.bleReady`), 'opening the system keyboard keeps Bluetooth ready and hides the fallback keyboard');
  await evaluate(`delete visualViewport.height;visualViewport.dispatchEvent(new Event('resize'));`);
  await evaluate(`globalThis.pressPageKey=(label)=>[...document.querySelectorAll('#field-keyboard button')].find(button=>button.textContent===label).click();
    pressPageKey('ABC');pressPageKey('A');pressPageKey('abc');pressPageKey('b');pressPageKey('1');
    pressPageKey('記号');pressPageKey('!');pressPageKey('空白');pressPageKey('削除');`);
  check(await evaluate('document.activeElement.id==="wifi-password" && document.getElementById("wifi-password").value==="Ab1!" && wifiDirty'),
    'the page keyboard enters a password with case, numbers, symbols and deletion');
  const beforeSwitch=inputModeRequests.length;
  await evaluate(`document.getElementById('passcode').focus()`);
  await new Promise(resolve=>setTimeout(resolve,350));
  check(inputModeRequests.slice(beforeSwitch).every(request=>request.enabled) &&
    await evaluate('document.activeElement.id==="passcode" && currentStatus.inputModePaused && currentStatus.bleConnected'),
    'moving between fields keeps key output paused with Bluetooth connected');
  await wait('!document.getElementById("field-keyboard").hidden');
  await evaluate(`pressPageKey('0');pressPageKey('2');pressPageKey('4');pressPageKey('6');`);
  check(await evaluate('document.getElementById("passcode").value==="0246" && passcodeDirty && !document.querySelector("#field-keyboard button").form.querySelector("#wifi-password")'),
    'the numeric keypad enters a passcode including leading zeroes');
  await evaluate(`pressPageKey('完了')`);
  await wait('!currentStatus.inputModePaused && !inputModeRequest');
  check(await evaluate('currentStatus.bleReady && document.getElementById("input-mode-help").hidden && document.getElementById("field-keyboard").hidden'),'finishing input closes the keypad and resumes key output');
  delayInputMode=true;
  const beforeQuickBlur=inputModeRequests.length;
  await evaluate(`document.getElementById('passcode').focus();document.getElementById('passcode').blur()`);
  await wait('!inputModeDesired && !inputModeRequest');
  check(inputModeRequests.slice(beforeQuickBlur).map(request=>request.enabled).join(',')==='true,false',
    'a blur during a pending pause still sends a resume');
  delayInputMode=false;
  await evaluate(`document.getElementById('passcode').focus()`);
  await wait('currentStatus.inputModePaused && !inputModeRequest');
  await evaluate(`window.dispatchEvent(new PageTransitionEvent('pagehide'))`);
  await wait('!inputModeRequested && !inputModeRequest');
  check(!inputModePaused && inputModeRequests.at(-1).enabled===false,'leaving the page releases the pause');
  await evaluate(`document.activeElement.blur();window.dispatchEvent(new PageTransitionEvent('pageshow'));`);
  await evaluate(`document.getElementById('ble-name-input').focus()`);
  await wait('currentStatus.inputModePaused && !document.getElementById("field-keyboard").hidden');
  await evaluate(`renderAll()`);
  await wait('!currentStatus.inputModePaused && !inputModeRequest');
  check(await evaluate('document.getElementById("field-keyboard").hidden && currentStatus.bleReady'),
    'replacing an edited field returns the fallback keyboard and resumes output without reconnecting');
  shot=await call('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});await writeFile(join(output,'setup-direct.png'),Buffer.from(shot.data,'base64'));
  await evaluate(`document.getElementById('discard').click()`);
  check(await evaluate(`document.getElementById('firmware-version').textContent==='1.0.1' &&
    document.getElementById('firmware-repository').textContent==='suzuking001/steering_sw' &&
    document.getElementById('firmware-check').disabled && !document.querySelector('#firmware-panel input')`),
    'firmware panel shows the installed version and the fixed repository');
  wifiState='connected';
  await wait('!document.getElementById("firmware-check").disabled');
  await evaluate(`document.getElementById('firmware-check').click()`);
  await wait('currentStatus.firmware.stage==="checking"');
  check(!firmware.tokenSet && firmwareRequests.join(',')==='firmware/check' && await evaluate('document.getElementById("firmware-install").hidden && document.getElementById("connection").textContent==="BLE 一時停止"'),
    'checking without a token shows the temporary Bluetooth pause and does not begin installation');
  Object.assign(firmware,{stage:'failed',message:'公開リリースが見つかりません。更新元が非公開の場合はGitHubの認証が必要です。'});
  await wait('document.getElementById("firmware-status").textContent.includes("公開リリースが見つかりません")');
  check(await evaluate('!document.getElementById("firmware-check").disabled && document.getElementById("firmware-install").hidden'),
    'an unavailable public release shows an error and leaves the check button enabled');
  await evaluate(`document.getElementById('firmware-check').click()`);
  await wait('currentStatus.firmware.stage==="checking"');
  Object.assign(firmware,{stage:'available',message:'',availableVersion:'1.0.2',size:1500000});
  await wait('!document.getElementById("firmware-install").hidden');
  await evaluate(`setDirty();document.getElementById('firmware-install').click()`);
  check(firmwareRequests.length===2 && await evaluate('document.getElementById("firmware-request-feedback").textContent.includes("未保存")'),
    'unsaved edits are protected from an update restart');
  await evaluate(`markSaved();document.getElementById('firmware-install').click()`);
  await wait('currentStatus.firmware.stage==="downloading"');
  firmware.received=750000;
  await wait('document.getElementById("firmware-progress").value===50');
  check(firmwareRequests.at(-1)==='1.0.2' && await evaluate('!document.getElementById("firmware-progress").hidden && document.getElementById("firmware-check").disabled'),
    'an explicit install targets the checked version and shows download progress');
  Object.assign(firmware,{stage:'idle',version:'1.0.2',availableVersion:'',received:0,size:0,bootChecking:true});
  await wait('document.getElementById("firmware-status").textContent.includes("起動を確認")');
  firmware.bootChecking=false;
  await wait('document.getElementById("firmware-status").textContent.includes("更新が完了")');
  check(await evaluate('document.getElementById("firmware-version").textContent==="1.0.2"'),
    'success is shown after the new version reports a healthy boot');
  check(errors.length===0,'no uncaught browser or fixture errors');
  console.log(`${checks} browser checks passed; screenshots: .pio/ui-tests/`);
}catch(error){console.error(error);if(errors.length)console.error(errors);process.exitCode=1;}
finally{if(ws)ws.close();if(chrome){chrome.kill('SIGTERM');await once(chrome,'exit').catch(()=>{});}server.closeAllConnections();server.close();await rm(profile,{recursive:true,force:true});}
function cloneJSON(value){return JSON.parse(JSON.stringify(value));}
