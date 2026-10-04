'use strict';
let response='',body='{}',received=false;
global.uhttpd={send:s=>{response+=s;},recv:n=>{if(received)return '';received=true;return body;},urldecode:s=>s};
loadfile('/usr/share/e5-infoscreen/api.uc',{raw_mode:false})();
function get(path) {
 response='';received=false;
 global.handle_request({PATH_INFO:path,REQUEST_METHOD:'GET',CONTENT_LENGTH:0});
 let offset=index(response,'\r\n\r\n');assert(offset>=0,'missing API response');
 let result=json(substr(response,offset+4));
 assert(type(result.error)!='string',sprintf('API error at %s: %J',path,result));
 return result;
}
let result=get('/status');assert(type(result.usb.reset)=='object','USB recovery status missing');
result=get('/settings/usb');assert(type(result.items)=='array','USB settings unavailable');
assert(length(filter(result.items,i=>i.id=='check'))==1,'diagnostic action unavailable');
print('Real network API request checks passed\n');
