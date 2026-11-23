(() => {
  const loader = document.querySelector('.loader');
  let dismissed = false;
  const dismiss = () => {
    if (dismissed) return;
    dismissed = true;
    loader.classList.add('done');
    document.body.classList.add('ready');
  };
  loader.querySelector('button').addEventListener('click', dismiss);
  window.addEventListener('keydown', event => {
    if (event.key === 'Escape') dismiss();
  });
  window.setTimeout(dismiss, 1650);
})();
