// The native OBS locale lookup starts with en-US.ini and overlays the selected
// language. Keep English in the HTML and in dynamic fallbacks until it arrives.
export function translate(labels, key, fallback, ...args) {
  let value = labels?.[key] || fallback;
  for (let index = 0; index < args.length; ++index)
    value = value.replaceAll(`%${index + 1}`, String(args[index]));
  return value;
}

const englishFallbacks = new WeakMap();

export function applyLocalization(root, labels, locale) {
  root.documentElement.lang = (locale || "en-US").replaceAll("_", "-");
  for (const node of root.querySelectorAll("[data-i18n], [data-i18n-title], [data-i18n-aria-label]")) {
    for (const [attribute, property] of [["data-i18n", "textContent"], ["data-i18n-title", "title"], ["data-i18n-aria-label", "aria-label"]]) {
      const key = node.getAttribute(attribute);
      if (!key) continue;
      let fallbacks = englishFallbacks.get(node);
      if (!fallbacks) { fallbacks = {}; englishFallbacks.set(node, fallbacks); }
      if (!(attribute in fallbacks)) fallbacks[attribute] = property === "textContent" ? node.textContent : node.getAttribute(property);
      const fallback = fallbacks[attribute];
      if (property === "textContent") node.textContent = translate(labels, key, fallback);
      else node.setAttribute(property, translate(labels, key, fallback));
    }
  }
}
