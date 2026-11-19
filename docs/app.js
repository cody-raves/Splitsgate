const steps=[...document.querySelectorAll('.tutorial-step')];
const scenes=[...document.querySelectorAll('.scene')];
const toggle=document.querySelector('#tutorial-toggle');
let active=0,playing=!matchMedia('(prefers-reduced-motion: reduce)').matches,timer;
function showStep(index){
  active=index;
  steps.forEach((step,i)=>{const on=i===index;step.classList.toggle('active',on);step.setAttribute('aria-selected',String(on));});
  scenes.forEach((scene,i)=>scene.classList.toggle('active',i===index));
}
function schedule(){clearInterval(timer);if(playing)timer=setInterval(()=>showStep((active+1)%steps.length),4600);}
steps.forEach((step,i)=>step.addEventListener('click',()=>{showStep(i);schedule();}));
toggle.addEventListener('click',()=>{playing=!playing;toggle.textContent=playing?'Pause animation':'Play animation';toggle.setAttribute('aria-label',toggle.textContent);schedule();});
showStep(0);schedule();

fetch('https://api.github.com/repos/cody-raves/Splitsgate/releases/latest',{headers:{Accept:'application/vnd.github+json'}})
  .then(response=>response.ok?response.json():Promise.reject())
  .then(release=>{
    const zip=release.assets?.find(asset=>asset.name==='Splitsgate.zip')||release.assets?.find(asset=>asset.name.endsWith('.zip'));
    if(zip)document.querySelectorAll('#download-primary,.download-mirror').forEach(link=>link.href=zip.browser_download_url);
    if(release.tag_name)document.querySelector('#release-label').textContent=`${release.tag_name} · Windows 10/11 · ${zip?'release zip':'latest release'}`;
  }).catch(()=>{});
