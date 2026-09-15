"""Mirror of the scene-cut path in ``src/TemporalGuides.cpp``.

Shared by ``analyze.py``, which scores the detector inside a rendered run, and by
``cutlab.py``, which scores the criterion itself against labelled ground truth. The
worth of this module is that it is *not* an independent implementation: the analysis
grid, the stratified downsample, the normalized Rec.709 cell luma, the global search
with its distance penalty and its refusal to prefer a marginal shift, the 3x3 patch
cost, the thresholds and the weak-arm debounce are all the generator's own. A threshold
swept here therefore transfers to C++ unchanged, and a disagreement between the two is
a bug in one of them rather than a difference of opinion.
"""
from __future__ import annotations

import math

import numpy as np

# --- shipped criterion (TemporalGuides.cpp ClassifySceneCut) -------------------------
CUT_RESIDUAL_STRONG, CUT_RESIDUAL_WEAK, CUT_HISTOGRAM_OVERLAP = 0.30, 0.10, 0.85
MIN_SECONDS_BETWEEN_CUTS = 0.3
CUT_MATCH_FRAMES = 1

# --- scale-free candidate (roadmap survey item 3) ------------------------------------
# A cell's match failed when its winning displacement cannot explain the change: the
# winner's cost has to come in under CUT_MATCH_RATIO of the cost of standing still,
# which is the no-prediction baseline EstimateFlow already computes and discards. A
# cell whose standing-still cost is below CUT_ACTIVE_COST has nothing to explain and
# is never counted as a failure. The decision is then the *fraction* of failed cells
# (mvtools thSCD2) instead of a mean residual, which is what makes it scale-free.
#
# These four are the best point cutlab.py's sweep found for this family over the
# labelled corpus, not a shipped threshold: nothing in src/ reads them. The weak arm
# needs no histogram gate at this point, which is the one thing the candidate does
# better than the residual - and it still buys no decision the residual cannot reach.
CUT_MATCH_RATIO = 0.80
CUT_ACTIVE_COST = 0.08
CUT_FAILED_STRONG, CUT_FAILED_WEAK = 0.80, 0.20
REC709 = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32) / 255

GLOBAL_RADIUS = 7
LOCAL_RADIUS = 3


def analysis_grid(width: int, height: int, fps: float) -> tuple[int, int]:
    """TemporalGuideGenerator::AnalysisGrid - the cell field the guide solver works on."""
    high_fps = fps >= 45.0
    gw = min(max(width // (14 if high_fps else 10), 96), 128 if high_fps else 160)
    return gw, max(48 if high_fps else 54, gw * height // width)


class GuideGrid:
    """Reduces a frame to one Rec.709 luma value per analysis cell.

    Four stratified samples per cell, which is DownsampleLuma's own scheme, so a verdict
    about a cell here is a verdict about the cell the worker estimates a vector for.
    """

    def __init__(self, width: int, height: int, fps: float):
        self.gw, self.gh = analysis_grid(width, height, fps)
        self.cell_w, self.cell_h = width / self.gw, height / self.gh
        self.rows, self.cols = self.taps(height, self.gh), self.taps(width, self.gw)

    @staticmethod
    def taps(size: int, cells: int) -> tuple[np.ndarray, np.ndarray]:
        first = np.arange(cells) * size // cells
        last = np.maximum(first + 1, (np.arange(cells) + 1) * size // cells)
        return first, np.minimum(size - 1, (first + last) // 2)

    def cells(self, rgb: np.ndarray) -> np.ndarray:
        return sum(rgb[np.ix_(ys, xs)] @ REC709 for ys in self.rows for xs in self.cols) * 0.25


def global_search(cur: np.ndarray, prev: np.ndarray) -> tuple[float, int, int]:
    """EstimateFlow's whole-frame translation: its cost, and the vector the cells search around.

    The cheapest of the 225 shifts within +/-7 cells by mean |dY| over a 4-cell lattice,
    with the same quadratic distance penalty and the same refusal to prefer a marginal
    shift over standing still.
    """
    gh, gw = cur.shape
    rows, cols = np.arange(4, gh - 4, 4), np.arange(4, gw - 4, 4)
    if not rows.size or not cols.size:
        return 0.0, 0, 0
    patch = cur[np.ix_(rows, cols)]
    best = zero = None
    gx = gy = 0
    for dy in range(-GLOBAL_RADIUS, GLOBAL_RADIUS + 1):
        keep_y = (rows + dy >= 0) & (rows + dy < gh)
        for dx in range(-GLOBAL_RADIUS, GLOBAL_RADIUS + 1):
            keep_x = (cols + dx >= 0) & (cols + dx < gw)
            cost = 0.0 if not (keep_y.any() and keep_x.any()) else float(np.abs(
                patch[np.ix_(keep_y, keep_x)] - prev[np.ix_(rows[keep_y] + dy, cols[keep_x] + dx)]).mean())
            cost += 0.0015 * (dx * dx + dy * dy)
            if dx == 0 and dy == 0:
                zero = cost
            if best is None or cost < best:
                best, gx, gy = cost, dx, dy
    if (gx or gy) and zero - best < 0.012:
        return zero, 0, 0
    return best, gx, gy


def histogram_overlap(cur: np.ndarray, prev: np.ndarray) -> float:
    """LumaHistogramIntersection: normalized 32-bin intersection of two cell-luma histograms."""
    counts = [np.bincount(np.clip(grid.ravel() * 32, 0, 31).astype(np.int32), minlength=32)
              for grid in (cur, prev)]
    return float(np.minimum(*counts).sum() / cur.size)


def _patch_cost(cur: np.ndarray, prev: np.ndarray, ys: np.ndarray, xs: np.ndarray,
                dy: int, dx: int, penalty: float) -> np.ndarray:
    """PatchSadCost over a whole lattice at once: mean |dY| of the valid 3x3 taps, plus penalty.

    Taps whose current or reference position leaves the grid are dropped and the divisor
    is the surviving count, exactly as the C++ does at the border.
    """
    gh, gw = cur.shape
    sad = np.zeros((ys.size, xs.size), dtype=np.float64)
    count = np.zeros((ys.size, xs.size), dtype=np.int32)
    for py in (-1, 0, 1):
        cy, oy = ys + py, ys + py + dy
        row_ok = ((cy >= 0) & (cy < gh) & (oy >= 0) & (oy < gh))[:, None]
        cyc, oyc = np.clip(cy, 0, gh - 1), np.clip(oy, 0, gh - 1)
        for px in (-1, 0, 1):
            cx, ox = xs + px, xs + px + dx
            ok = row_ok & ((cx >= 0) & (cx < gw) & (ox >= 0) & (ox < gw))[None, :]
            cxc, oxc = np.clip(cx, 0, gw - 1), np.clip(ox, 0, gw - 1)
            diff = np.abs(cur[cyc[:, None], cxc[None, :]] - prev[oyc[:, None], oxc[None, :]])
            sad += np.where(ok, diff, 0.0)
            count += ok
    return np.where(count > 0, sad / np.maximum(count, 1), 10.0) + penalty


def cell_match_costs(cur: np.ndarray, prev: np.ndarray, gx: int, gy: int) -> tuple[np.ndarray, np.ndarray]:
    """EstimateFlow's per-cell landscape, reduced to the two numbers a cut decision needs.

    ``best`` is the winner of the +-3 cell window centred on the global vector, penalty
    included; ``zero`` is that cell's standing-still cost, read out of the same window
    when it falls inside it and computed unpenalized when it does not - which is where
    the C++ reads its motion evidence from. Both are on EstimateFlow's 2x2 lattice, so a
    fraction computed over them is a fraction over the cells the solver actually solves.
    """
    gh, gw = cur.shape
    ys, xs = np.arange(0, gh, 2), np.arange(0, gw, 2)
    best = None
    zero = None
    for oy in range(-LOCAL_RADIUS, LOCAL_RADIUS + 1):
        for ox in range(-LOCAL_RADIUS, LOCAL_RADIUS + 1):
            cost = _patch_cost(cur, prev, ys, xs, gy + oy, gx + ox, 0.002 * (ox * ox + oy * oy))
            best = cost if best is None else np.minimum(best, cost)
            if gx + ox == 0 and gy + oy == 0:
                zero = cost
    if zero is None:
        zero = _patch_cost(cur, prev, ys, xs, 0, 0, 0.0)
    return best.astype(np.float32), zero.astype(np.float32)


def failed_fraction(best: np.ndarray, zero: np.ndarray,
                    ratio: float = CUT_MATCH_RATIO, floor: float = CUT_ACTIVE_COST) -> float:
    """Fraction of all lattice cells whose best displacement failed to explain the change."""
    return float(((zero > floor) & (best > ratio * zero)).mean())


def pair_features(cur: np.ndarray, prev: np.ndarray, cells: bool = False) -> dict:
    """Everything either criterion reads from one consecutive pair of cell grids."""
    residual, gx, gy = global_search(cur, prev)
    feature = dict(residual=residual, histogram_overlap=histogram_overlap(cur, prev),
                   global_x=gx, global_y=gy)
    if cells:
        feature["cell_best"], feature["cell_zero"] = cell_match_costs(cur, prev, gx, gy)
    return feature


class Criterion:
    """A scene-cut test: ``None``/``Histogram``/``Residual`` from one pair's features."""

    NONE, WEAK, STRONG = 0, 1, 2
    ARMS = ("none", "histogram", "residual")
    needs_cells = False

    def classify(self, feature: dict) -> int:
        raise NotImplementedError

    def score(self, feature: dict) -> float:
        """The quantity the strong/weak thresholds are applied to, for reporting."""
        raise NotImplementedError


class ResidualCriterion(Criterion):
    """The shipped test: an absolute residual, with a histogram gate under the strong arm."""

    name = "residual"

    def __init__(self, strong: float = CUT_RESIDUAL_STRONG, weak: float = CUT_RESIDUAL_WEAK,
                 overlap: float = CUT_HISTOGRAM_OVERLAP):
        self.strong, self.weak, self.overlap = strong, weak, overlap

    def __str__(self) -> str:
        return f"residual>{self.strong:g} | residual>{self.weak:g} & overlap<{self.overlap:g}"

    def score(self, feature: dict) -> float:
        return feature["residual"]

    def classify(self, feature: dict) -> int:
        residual = feature["residual"]
        if residual > self.strong:
            return self.STRONG
        if residual > self.weak and feature["histogram_overlap"] < self.overlap:
            return self.WEAK
        return self.NONE


class FailedFractionCriterion(Criterion):
    """Roadmap item 3: the fraction of cells whose match failed against no prediction."""

    name = "failed-fraction"
    needs_cells = True

    def __init__(self, strong: float = CUT_FAILED_STRONG, weak: float = CUT_FAILED_WEAK,
                 ratio: float = CUT_MATCH_RATIO, floor: float = CUT_ACTIVE_COST,
                 overlap: float | None = None):
        self.strong, self.weak, self.ratio, self.floor, self.overlap = strong, weak, ratio, floor, overlap

    def __str__(self) -> str:
        gate = "" if self.overlap is None else f" & overlap<{self.overlap:g}"
        return (f"failed>{self.strong:g} | failed>{self.weak:g}{gate} "
                f"(ratio {self.ratio:g}, floor {self.floor:g})")

    def score(self, feature: dict) -> float:
        # A sweep asks the same (ratio, floor) of the same pair once per threshold pair,
        # and the fraction costs a pass over every cell. Keep it on the pair.
        key = ("failed", self.ratio, self.floor)
        value = feature.get(key)
        if value is None:
            value = feature[key] = failed_fraction(feature["cell_best"], feature["cell_zero"],
                                                   self.ratio, self.floor)
        return value

    def classify(self, feature: dict) -> int:
        fraction = self.score(feature)
        if fraction > self.strong:
            return self.STRONG
        if fraction > self.weak and (self.overlap is None or feature["histogram_overlap"] < self.overlap):
            return self.WEAK
        return self.NONE


def min_frames_between_cuts(fps: float, seconds: float = MIN_SECONDS_BETWEEN_CUTS) -> int:
    """MinFramesBetweenCuts. ``math.floor(x + 0.5)`` is what C++ ``std::lround`` does;
    Python's ``round`` breaks ties to even and would disagree on a half-frame window."""
    return max(2, math.floor(seconds * fps + 0.5)) if fps > 0 else 2


class CutRun:
    """Generate()'s cut branch replayed over one stream: debounce, resets and evidence.

    Frame 0 is the generator's FirstFrame reset rather than a detection; every later
    frame is judged against its predecessor, so ``cuts`` is the sequence of history
    resets image evidence alone would produce and ``evidence`` is one row per decision.
    """

    def __init__(self, fps: float, criterion: Criterion | None = None,
                 seconds_between_cuts: float = MIN_SECONDS_BETWEEN_CUTS):
        self.criterion = criterion or ResidualCriterion()
        self.min_frames = min_frames_between_cuts(fps, seconds_between_cuts)
        self.since_cut, self.accepted_any = 0, False
        self.cuts: list[int] = []
        self.suppressed: list[int] = []
        self.evidence: list[dict] = []

    def decide(self, index: int, feature: dict) -> int:
        self.since_cut += 1
        strength = self.criterion.classify(feature)
        if strength == Criterion.NONE:
            return strength
        withheld = strength == Criterion.WEAK and self.accepted_any and self.since_cut < self.min_frames
        self.evidence.append(dict(frame=index, residual=feature["residual"],
                                  histogram_overlap=feature["histogram_overlap"],
                                  score=self.criterion.score(feature),
                                  arm=Criterion.ARMS[strength], suppressed=withheld))
        if withheld:
            self.suppressed.append(index)
        else:
            self.cuts.append(index)
            self.since_cut, self.accepted_any = 0, True
        return strength

    def feed(self, index: int, cur: np.ndarray, prev: np.ndarray) -> None:
        self.decide(index, pair_features(cur, prev, cells=self.criterion.needs_cells))


def cut_scores(detected: list[int], truth: list[int], tolerance: int = CUT_MATCH_FRAMES,
               soft_cuts: list | None = None) -> dict:
    """Precision/recall/F1 against the manifest's hard-cut indices.

    At most one detection matches each ground-truth cut, within ``tolerance`` frames of
    it. A cut-free clip has no recall to report and its detection count is its false
    positives. ``soft_cuts`` are spans - a dissolve, where there is no single correct
    frame - inside which the first reset is neither a hit nor a false positive; a second
    reset inside the same span is still a false positive, because re-resetting inside one
    transition is the artifact the debounce exists to prevent.
    """
    spare = sorted(detected)
    matched = 0
    for cut in sorted(truth):
        near = [d for d in spare if abs(d - cut) <= tolerance]
        if near:
            spare.remove(min(near, key=lambda d: (abs(d - cut), d)))
            matched += 1
    tolerated = 0
    for first, last in soft_cuts or []:
        inside = [d for d in spare if first <= d <= last]
        if inside:
            spare.remove(inside[0])
            tolerated += 1
    scored = len(detected) - tolerated
    precision = matched / scored if scored else None
    recall = matched / len(truth) if truth else None
    f1 = None if precision is None or recall is None else \
        (0.0 if precision + recall == 0 else 2 * precision * recall / (precision + recall))
    return dict(detected=sorted(detected), matched=matched, missed=len(truth) - matched,
                tolerated=tolerated, false_positives=spare, precision=precision, recall=recall, f1=f1)
