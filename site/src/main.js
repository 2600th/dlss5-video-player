/* =============================================================================
   DLSS 5 Video Player — project site

   Three jobs: drive the comparison seam, load the demonstration only when asked,
   and keep the download honest if this page has been sitting on a CDN since
   before the current release. Everything here is an enhancement: with scripting
   off, both frames are still on the page, the seam sits at its authored
   position, and every download link resolves.
   ========================================================================== */

(function () {
  'use strict';

  var reduceMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

  /* --- analytics ---------------------------------------------------------- */

  function track(name, params) {
    if (typeof window.gtag !== 'function') { return; }
    try { window.gtag('event', name, params || {}); } catch (e) { /* never break the page for a metric */ }
  }

  /* --- the seam ----------------------------------------------------------- */

  var compare = document.getElementById('compare');
  var range = document.getElementById('seam');

  if (compare && range) {
    var draggedOnce = false;

    var apply = function (value) {
      compare.style.setProperty('--seam', value + '%');
    };

    apply(parseFloat(range.value));

    range.addEventListener('input', function () {
      apply(parseFloat(range.value));
      if (!draggedOnce) {
        draggedOnce = true;
        track('compare_drag', { surface: 'hero' });
      }
    });

    // The one authored moment: the original holds the frame, then the render
    // sweeps in and stops off-centre so the split is visible before anyone
    // touches it. Exponential ease-out, once, from an already-visible state.
    if (!reduceMotion) {
      var settle = parseFloat(range.value);
      var start = 88;
      var duration = 1500;
      var began = null;

      apply(start);
      range.value = String(start);

      var step = function (now) {
        if (began === null) { began = now; }
        var t = Math.min((now - began) / duration, 1);
        var eased = 1 - Math.pow(2, -10 * t);
        if (t >= 1) { eased = 1; }
        var value = start + (settle - start) * eased;
        apply(value);
        range.value = String(value);
        if (t < 1) { window.requestAnimationFrame(step); }
      };

      window.setTimeout(function () { window.requestAnimationFrame(step); }, 550);
    }
  }

  /* --- the demonstration -------------------------------------------------- */

  var demo = document.querySelector('.demo');

  if (demo) {
    var trigger = demo.querySelector('.demo__play');
    if (trigger) {
      trigger.addEventListener('click', function () {
        var source = demo.getAttribute('data-video');
        if (!source) { return; }

        var video = document.createElement('video');
        video.src = source;
        video.controls = true;
        video.autoplay = true;
        video.playsInline = true;
        video.setAttribute('poster', 'assets/demo/demo-poster.jpg');

        demo.textContent = '';
        demo.appendChild(video);
        video.focus({ preventScroll: true });
        track('demo_play', { length_seconds: 22 });
      });
    }
  }

  /* --- checksums ---------------------------------------------------------- */

  document.querySelectorAll('button.pkg__sum').forEach(function (button) {
    button.addEventListener('click', function () {
      var value = button.getAttribute('data-checksum');
      if (!value || !navigator.clipboard) { return; }
      navigator.clipboard.writeText(value).then(function () {
        var action = button.querySelector('.pkg__sum-action');
        if (!action) { return; }
        var original = action.textContent;
        action.textContent = 'copied';
        button.setAttribute('data-copied', '1');
        window.setTimeout(function () {
          action.textContent = original;
          button.removeAttribute('data-copied');
        }, 1800);
      }, function () { /* clipboard refused; the hash is still on screen */ });
    });
  });

  /* --- downloads ---------------------------------------------------------- *
   * The click is acknowledged because nothing else acknowledges it. A release
   * asset is served cross-origin, so the page is told nothing about the
   * download - not its progress, not its completion, not even that it began -
   * and the complete package is over 300 MB, which is long enough for the
   * browser's own indicator to feel late. So the link relabels itself, says so
   * once for a screen reader, and puts itself back.
   *
   * Deliberately not a progress bar or a spinner: there is no progress to read
   * here, and inventing one would be lying about a transfer this page cannot
   * see. The link keeps its ordinary behaviour throughout - nothing is
   * prevented, and with scripting off the download is exactly as it was.
   * ---------------------------------------------------------------------- */

  var ACKNOWLEDGE_MS = 6000;
  var status = document.querySelector('.download-status');

  document.querySelectorAll('[data-download]').forEach(function (link) {
    // Whichever slot this link keeps its size in; both say the same thing.
    var slot = link.querySelector('.button__meta, .pkg__cta-size');
    var original = null;
    var restore = null;

    link.addEventListener('click', function () {
      track('download_click', {
        package: link.getAttribute('data-download'),
        version: document.documentElement.getAttribute('data-version') || 'unknown'
      });

      if (!slot) { return; }

      // Read at click rather than at setup: the backstop below can rewrite the
      // hero's meta after this handler is attached, and restoring a string
      // captured before that would erase its notice. A second click restarts
      // the window rather than stacking timers, and must not capture the
      // message already sitting in the slot as the thing to restore.
      if (restore) { window.clearTimeout(restore); }
      else { original = slot.textContent; }

      slot.textContent = 'starting - check your downloads';
      link.setAttribute('data-downloading', '1');
      if (status) { status.textContent = 'Download starting. Check your browser downloads.'; }

      restore = window.setTimeout(function () {
        slot.textContent = original;
        link.removeAttribute('data-downloading');
        if (status) { status.textContent = ''; }
        restore = null;
      }, ACKNOWLEDGE_MS);
    });
  });

  /* --- the backstop ------------------------------------------------------- *
   * Publishing a release dispatches the deploy, so the baked release data is
   * normally right within a minute. This exists for the run that failed.
   * It is deliberately timid: one request per session, only when the build is
   * already a day old, and any surprise at all leaves the page as built.
   * ----------------------------------------------------------------------- */

  var DAY = 24 * 60 * 60 * 1000;

  function refreshRelease() {
    var flag = 'dlss5-release-checked';
    try { if (window.sessionStorage.getItem(flag)) { return; } } catch (e) { return; }

    fetch('release.json', { cache: 'no-cache' }).then(function (response) {
      return response.ok ? response.json() : null;
    }).then(function (baked) {
      if (!baked || !baked.builtAt || !baked.repo) { return; }
      if (Date.now() - Date.parse(baked.builtAt) < DAY) { return; }

      try { window.sessionStorage.setItem(flag, '1'); } catch (e) { /* private mode */ }

      return fetch('https://api.github.com/repos/' + baked.repo + '/releases?per_page=10')
        .then(function (response) { return response.ok ? response.json() : null; })
        .then(function (releases) {
          if (!Array.isArray(releases)) { return; }

          var current = releases.filter(function (release) { return !release.draft; })
            .sort(function (a, b) { return Date.parse(b.published_at) - Date.parse(a.published_at); })[0];
          if (!current || !current.tag_name) { return; }

          var version = current.tag_name.replace(/^dlss5-video-player-v/, '');
          if (!version || version === baked.version) { return; }

          var asset = (current.assets || []).filter(function (a) {
            var name = (a.name || '').toLowerCase();
            return name.indexOf('-win64.zip') !== -1 &&
                   name.indexOf('.sha256') === -1 &&
                   name.indexOf('-core-win64.zip') === -1;
          })[0];

          announce(version, current.html_url, asset ? asset.browser_download_url : current.html_url);
        });
    }).catch(function () { /* offline, rate limited, or shape changed: keep the baked page */ });
  }

  function announce(version, releaseUrl, downloadUrl) {
    var hero = document.querySelector('.hero__actions .button--primary');
    if (hero) {
      hero.setAttribute('href', downloadUrl);
      var meta = hero.querySelector('.button__meta');
      if (meta) { meta.textContent = 'v' + version + ' · newer than this page'; }
    }

    var note = document.querySelector('#download .section__note');
    if (note) {
      var line = document.createElement('span');
      line.style.display = 'block';
      line.style.marginTop = '0.6rem';
      line.style.color = 'var(--flag)';
      var link = document.createElement('a');
      link.href = releaseUrl;
      link.rel = 'noopener';
      link.textContent = 'v' + version + ' has been published since this page was built.';
      line.appendChild(link);
      note.appendChild(line);
    }
  }

  refreshRelease();
})();
