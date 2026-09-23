(() => {
  'use strict';
  const params = new URLSearchParams(location.search);
  const token = params.get('token') || '';
  const generation = Number(params.get('generation'));
  const status = document.getElementById('status');
  const size = document.getElementById('size');
  const sizeOut = document.getElementById('sizeOut');
  let center = { lng: -121.474, lat: 46.935 };
  const probe = document.createElement('canvas');
  const hasWebGl = Boolean(probe.getContext('webgl2') || probe.getContext('webgl'));

  let map = null;
  if (hasWebGl) {
    map = new maplibregl.Map({
      container: 'map',
      center: [center.lng, center.lat],
      zoom: 11,
      attributionControl: false,
      style: {
        version: 8,
        sources: { base: { type: 'raster', tiles: ['https://tile.openstreetmap.org/{z}/{x}/{y}.png'], tileSize: 256 } },
        layers: [{ id: 'base', type: 'raster', source: 'base' }]
      }
    });
    const marker = new maplibregl.Marker({ color: '#ef7d4f' }).setLngLat(center).addTo(map);
    map.on('click', event => {
      center = event.lngLat;
      marker.setLngLat(center);
      status.textContent = `Center ${center.lat.toFixed(5)}, ${center.lng.toFixed(5)}`;
    });
  } else {
    status.textContent = 'Packaged CEF WebGL is unavailable.';
  }
  size.addEventListener('input', () => { sizeOut.textContent = `${size.value} km`; });

  const submitSelection = () => {
    const kilometers = Number(size.value);
    const halfLat = kilometers / 2 / 111.32;
    const halfLng = kilometers / 2 / (111.32 * Math.cos(center.lat * Math.PI / 180));
    const request = {
      token,
      generation,
      name: document.getElementById('name').value,
      profile: document.getElementById('profile').value,
      west: center.lng - halfLng,
      south: center.lat - halfLat,
      east: center.lng + halfLng,
      north: center.lat + halfLat
    };
    status.textContent = 'Validating selection…';
    if (!window.ue || !window.ue.skiselector) {
      status.textContent = 'Native selector bridge unavailable.';
      return;
    }
    window.ue.skiselector.submit(JSON.stringify(request));
  };
  document.getElementById('selection').addEventListener('submit', event => {
    event.preventDefault();
    submitSelection();
  });
  if (params.get('autotest') === '1') {
    if (!hasWebGl) return;
    let attempts = 0;
    const waitForBridge = setInterval(() => {
      attempts += 1;
      if (window.ue && window.ue.skiselector && typeof window.ue.skiselector.submit === 'function') {
        clearInterval(waitForBridge);
        const popupProbe = document.createElement('a');
        popupProbe.href = 'https://example.com/blocked-popup';
        popupProbe.target = '_blank';
        document.body.appendChild(popupProbe);
        popupProbe.click();
        popupProbe.remove();
        location.assign('https://example.com/blocked-navigation');
        setTimeout(submitSelection, 100);
      } else if (attempts >= 50) {
        clearInterval(waitForBridge);
        status.textContent = 'Native selector bridge unavailable.';
      }
    }, 100);
  }
})();
