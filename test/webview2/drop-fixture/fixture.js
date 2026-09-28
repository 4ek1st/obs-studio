const rejected = new Set();
const reportRejected = id => {
  rejected.add(id);
  if (rejected.size === 3) chrome.webview.postMessage({version:1,id:'fixture-validation',command:'test.validation',args:{rejected:true}});
};
chrome.webview.addEventListener('message', ({data}) => {
  if (['fixture-forged','fixture-file-url','fixture-synthetic'].includes(data.id)) {
    if (data.ok) chrome.webview.postMessage({version:1,id:'fixture-validation',command:'test.validation',args:{rejected:false}});
    else reportRejected(data.id);
  }
  if (data.event !== 'test.drop') return;
  chrome.webview.postMessage({version:1,id:'fixture-text',command:'external.drop',args:{kind:'text',text:'External text from WebView2'}});
  chrome.webview.postMessage({version:1,id:'fixture-forged',command:'external.drop',args:{kind:'files',count:1,path:'C:\\Windows\\win.ini'}});
  chrome.webview.postMessage({version:1,id:'fixture-file-url',command:'external.drop',args:{kind:'urls',urls:['file:///C:/Windows/win.ini']}});
  try {
    chrome.webview.postMessageWithAdditionalObjects({version:1,id:'fixture-synthetic',command:'external.drop',args:{kind:'files',count:1}},[new File(['synthetic'], 'fake.png')]);
  } catch { reportRejected('fixture-synthetic'); }
});
document.getElementById('files').addEventListener('change',event => {
  const files=event.target.files;
  chrome.webview.postMessageWithAdditionalObjects({version:1,id:'fixture-file',command:'external.drop',args:{kind:'files',count:files.length}},files);
});
