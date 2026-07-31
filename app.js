/**
 * SKey Landing Page - Interactive Logic & UI Handler
 */

document.addEventListener('DOMContentLoaded', () => {
  document.documentElement.setAttribute('data-theme', 'dark');
  initTabs();
  initConfigGalleryTab();
  initCopyButtons();
  fetchLatestRelease();
});

/* Tab Switcher for Installation Guide */
function initTabs() {
  const tabButtons = document.querySelectorAll('.tab-btn');
  const tabPanes = document.querySelectorAll('.tab-pane');

  tabButtons.forEach(btn => {
    btn.addEventListener('click', () => {
      const targetId = btn.getAttribute('data-tab');

      tabButtons.forEach(b => b.classList.remove('active'));
      tabPanes.forEach(p => p.classList.remove('active'));

      btn.classList.add('active');
      const activePane = document.getElementById(targetId);
      if (activePane) {
        activePane.classList.add('active');
      }
    });
  });
}

/* Config Gallery Tab Switcher */
function initConfigGalleryTab() {
  const tabButtons = document.querySelectorAll('.config-tab-btn');
  const tabItems = document.querySelectorAll('.config-item');

  tabButtons.forEach(btn => {
    btn.addEventListener('click', () => {
      const targetId = btn.getAttribute('data-target');

      tabButtons.forEach(b => b.classList.remove('active'));
      tabItems.forEach(item => item.classList.remove('active'));

      btn.classList.add('active');
      const activeItem = document.getElementById(targetId);
      if (activeItem) {
        activeItem.classList.add('active');
      }
    });
  });
}

/* Copy Code to Clipboard with Fallback */
function initCopyButtons() {
  const copyBtns = document.querySelectorAll('.copy-btn');

  copyBtns.forEach(btn => {
    btn.addEventListener('click', (e) => {
      e.stopPropagation();
      const codeBlock = btn.parentElement ? btn.parentElement.querySelector('code') : null;
      if (!codeBlock) return;

      const textToCopy = codeBlock.innerText.trim();
      
      const onSuccess = () => {
        const originalText = btn.innerText;
        btn.innerText = '✓ Đã chép!';
        btn.classList.add('copied');

        setTimeout(() => {
          btn.innerText = originalText;
          btn.classList.remove('copied');
        }, 2000);
      };

      if (navigator.clipboard && window.isSecureContext) {
        navigator.clipboard.writeText(textToCopy).then(onSuccess).catch(() => fallbackCopy(textToCopy, onSuccess));
      } else {
        fallbackCopy(textToCopy, onSuccess);
      }
    });
  });
}

function fallbackCopy(text, callback) {
  const textArea = document.createElement('textarea');
  textArea.value = text;
  textArea.style.position = 'fixed';
  textArea.style.top = '-9999px';
  textArea.style.left = '-9999px';
  document.body.appendChild(textArea);
  textArea.focus();
  textArea.select();
  try {
    document.execCommand('copy');
    if (callback) callback();
  } catch (err) {
    console.error('Không thể chép lệnh:', err);
  }
  document.body.removeChild(textArea);
}

/* Fetch Latest Release from GitHub API */
async function fetchLatestRelease() {
  const container = document.getElementById('latestDebInfo');
  if (!container) return;

  const repoOwner = 'collyn';
  const repoName = 'skey';
  const fallbackVersion = '0.4.4';
  const fallbackFileName = `fcitx5-skey_${fallbackVersion}_amd64.deb`;
  const fallbackUrl = `https://github.com/${repoOwner}/${repoName}/releases/latest`;

  try {
    const res = await fetch(`https://api.github.com/repos/${repoOwner}/${repoName}/releases/latest`);
    if (!res.ok) throw new Error(`HTTP error! status: ${res.status}`);
    const data = await res.json();

    const debAsset = data.assets && data.assets.find(asset => asset.name.endsWith('.deb'));
    
    if (debAsset) {
      container.innerHTML = `Tải gói <a href="${debAsset.browser_download_url}" target="_blank" rel="noopener"><code>${debAsset.name}</code></a> (${data.tag_name}) từ GitHub Releases chính thức.`;
    } else if (data.tag_name) {
      container.innerHTML = `Tải gói <code>fcitx5-skey_${data.tag_name.replace(/^v/, '')}_amd64.deb</code> từ <a href="${data.html_url}" target="_blank" rel="noopener">GitHub Releases (${data.tag_name})</a> chính thức.`;
    } else {
      throw new Error('No release tag found');
    }
  } catch (err) {
    console.warn('Không thể lấy thông tin release từ GitHub API, sử dụng thông tin mặc định:', err);
    container.innerHTML = `Tải gói <a href="${fallbackUrl}" target="_blank" rel="noopener"><code>${fallbackFileName}</code></a> từ GitHub Releases chính thức.`;
  }
}

