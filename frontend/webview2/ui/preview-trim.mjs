export function previewTrimLimit(fullWidth, canvasHeight, aspect, renderedWidth, edge) {
  if (![fullWidth, canvasHeight, aspect, renderedWidth, edge].every(Number.isFinite) || aspect <= 0)
    return 0;
  // Preserve the current scene size. Once the blank sides are gone, the
  // handles stop instead of making OBS's fit-to-window preview scale down.
  const heightFitWidth = Math.max(0, canvasHeight - edge * 2) * aspect;
  return Math.max(0, Math.floor(fullWidth - Math.max(heightFitWidth, renderedWidth) - edge * 2 - 2));
}

export function clampPreviewTrims(left, right, limit, activeSide = "") {
  const maximum = Math.max(0, Math.floor(Number.isFinite(limit) ? limit : 0));
  let nextLeft = Math.max(0, Math.round(Number.isFinite(left) ? left : 0));
  let nextRight = Math.max(0, Math.round(Number.isFinite(right) ? right : 0));
  if (activeSide === "left") {
    nextRight = Math.min(nextRight, maximum);
    nextLeft = Math.min(nextLeft, maximum - nextRight);
  } else if (activeSide === "right") {
    nextLeft = Math.min(nextLeft, maximum);
    nextRight = Math.min(nextRight, maximum - nextLeft);
  } else if (nextLeft + nextRight > maximum) {
    const total = nextLeft + nextRight;
    nextLeft = Math.round(maximum * nextLeft / total);
    nextRight = maximum - nextLeft;
  }
  return {left: nextLeft, right: nextRight};
}
