import json
import math
import sys
from pathlib import Path

import cv2
import numpy as np


def quadratic(p0, p1, p2, count=500):
    t = np.linspace(0.0, 1.0, count, dtype=np.float32)[:, None]
    return (1.0 - t) ** 2 * p0 + 2.0 * (1.0 - t) * t * p1 + t**2 * p2


def curve_points(coords, move_x):
    sx = move_x / 300.0
    sy = move_x / 180.0
    left = np.array([coords["leftX"], coords["leftY"]], np.float32)
    right = np.array([coords["rightX"], coords["rightY"]], np.float32)
    ctrl1 = np.array([coords["ctrl1X"] * sx, coords["ctrl1Y"] * sy], np.float32)
    ctrl2 = np.array([coords["ctrl2X"] * sx, coords["ctrl2Y"] * sy], np.float32)
    joint = np.array([coords["jointX"] * sx, coords["jointY"] * sy], np.float32)
    return np.vstack((quadratic(left, ctrl1, joint), quadratic(joint, ctrl2, right)))


def polyline_mask(shape, points, thickness):
    mask = np.zeros(shape[:2], np.uint8)
    cv2.polylines(mask, [np.rint(points).astype(np.int32)], False, 255, thickness, cv2.LINE_AA)
    return mask


def normal_profile(gray, points, offsets=range(-12, 13)):
    sampled_points = points[::5]
    tangents = np.gradient(sampled_points, axis=0)
    lengths = np.linalg.norm(tangents, axis=1, keepdims=True)
    normals = np.column_stack((-tangents[:, 1], tangents[:, 0])) / np.maximum(lengths, 1e-6)
    values = []
    for offset in offsets:
        locations = sampled_points + normals * offset
        map_x = locations[:, 0].astype(np.float32).reshape(-1, 1)
        map_y = locations[:, 1].astype(np.float32).reshape(-1, 1)
        values.append(float(cv2.remap(gray, map_x, map_y, cv2.INTER_LINEAR,
                                      borderMode=cv2.BORDER_REPLICATE).mean()))
    return values


def main():
    snapshot = Path(sys.argv[1])
    response = json.loads((snapshot / "curve_response.json").read_text(encoding="utf-8"))
    original = cv2.imread(str(snapshot / "background_server_curve.jpg"), cv2.IMREAD_COLOR)
    if original is None:
        raise RuntimeError("background image could not be decoded")
    image = cv2.resize(original, (300, 180), interpolation=cv2.INTER_AREA)

    attr = response["data"]["data"]["attr1"].split(".")
    pair = lambda index: float(f"{attr[index]}.{attr[index + 1]}")
    coords = {
        "leftX": pair(0) * 300,
        "leftY": pair(2) * 180,
        "rightX": pair(4) * 300,
        "rightY": pair(6) * 180,
        "ctrl1X": pair(8) / 2,
        "ctrl1Y": pair(10) / 2,
        "ctrl2X": pair(12) / 2,
        "ctrl2Y": pair(14) / 2,
        "jointX": pair(16) / 2,
        "jointY": pair(18) / 2,
    }

    gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
    edges = cv2.Canny(gray, 55, 130)
    edge_distance = cv2.distanceTransform(255 - edges, cv2.DIST_L2, 3)
    scores = []
    pipe_scores = []

    for move_x in range(1, 243):
        points = curve_points(coords, move_x)
        center = polyline_mask(image.shape, points, 1)
        band = polyline_mask(image.shape, points, 11)
        inner = polyline_mask(image.shape, points, 5)
        rim = cv2.subtract(band, inner)

        center_values = gray[center > 0].astype(np.float32)
        rim_values = gray[rim > 0].astype(np.float32)
        center_contrast = float(center_values.mean() - rim_values.mean())

        # A five-pixel rendered stroke should have image edges close to both rims.
        rim_distance = float(edge_distance[rim > 0].mean())
        edge_density = float(np.count_nonzero(edges[rim > 0])) / max(1, np.count_nonzero(rim))
        score = center_contrast + 22.0 * edge_density - 2.2 * rim_distance
        scores.append((score, move_x, center_contrast, edge_density, rim_distance))

        profile = np.array(normal_profile(gray, points), np.float32)
        left_peak = float(profile[7:11].max())
        right_peak = float(profile[14:18].max())
        center_trough = float(profile[11:14].mean())
        outer = float(np.r_[profile[:5], profile[-5:]].mean())
        pipe_score = ((left_peak + right_peak) / 2.0 - center_trough
                      + (left_peak + right_peak) / 2.0 - outer
                      - 0.45 * abs(left_peak - right_peak))
        pipe_scores.append((pipe_score, move_x, left_peak, right_peak, center_trough, outer))

    scores.sort(reverse=True)
    pipe_scores.sort(reverse=True)
    high_gray = cv2.cvtColor(original, cv2.COLOR_BGR2GRAY)
    high_pipe_scores = []
    for move_x in range(1, 243):
        points = curve_points(coords, move_x) * 2.0
        profile = np.array(normal_profile(high_gray, points, range(-24, 25)), np.float32)
        left_peak = float(profile[15:22].max())
        right_peak = float(profile[27:34].max())
        center_trough = float(profile[22:27].mean())
        outer = float(np.r_[profile[:10], profile[-10:]].mean())
        pipe_score = ((left_peak + right_peak) / 2.0 - center_trough
                      + (left_peak + right_peak) / 2.0 - outer
                      - 0.45 * abs(left_peak - right_peak))
        high_pipe_scores.append((pipe_score, move_x, left_peak, right_peak, center_trough, outer))
    high_pipe_scores.sort(reverse=True)
    best_move = high_pipe_scores[0][1]

    candidates = list(range(180, 243, 10)) + [best_move]
    candidates = sorted(set(candidates))
    tiles = []
    for move_x in candidates:
        tile = image.copy()
        points = curve_points(coords, move_x)
        cv2.polylines(tile, [np.rint(points).astype(np.int32)], False, (0, 0, 255), 1, cv2.LINE_AA)
        cv2.putText(tile, f"moveX={move_x}", (6, 174), cv2.FONT_HERSHEY_SIMPLEX, 0.42,
                    (0, 255, 255), 1, cv2.LINE_AA)
        tiles.append(tile)
    while len(tiles) % 4:
        tiles.append(np.zeros_like(image))
    rows = [np.hstack(tiles[index:index + 4]) for index in range(0, len(tiles), 4)]
    cv2.imwrite(str(snapshot / "curve3_fit_grid.png"), np.vstack(rows))

    overlay = image.copy()
    best_points = curve_points(coords, best_move)
    cv2.polylines(overlay, [np.rint(best_points).astype(np.int32)], False, (0, 0, 255), 1, cv2.LINE_AA)
    cv2.imwrite(str(snapshot / "curve3_best_fit.png"), overlay)

    high_overlay = original.copy()
    cv2.polylines(high_overlay, [np.rint(curve_points(coords, best_move) * 2).astype(np.int32)],
                    False, (0, 0, 255), 2, cv2.LINE_AA)
    cv2.imwrite(str(snapshot / "curve3_best_fit_600x360.png"), high_overlay)

    print(json.dumps({
        "coordinates": coords,
        "bestMoveX": best_move,
        "bestRatioX": best_move / 300.0,
        "bestRatioY": best_move / 180.0,
        "topScores": [
            {
                "moveX": move_x,
                "score": round(score, 4),
                "centerContrast": round(contrast, 4),
                "edgeDensity": round(density, 4),
                "rimDistance": round(distance, 4),
            }
            for score, move_x, contrast, density, distance in scores[:15]
        ],
        "topPipeScores": [
            {
                "moveX": move_x,
                "score": round(score, 4),
                "leftPeak": round(left, 4),
                "rightPeak": round(right, 4),
                "center": round(center, 4),
                "outer": round(outer, 4),
            }
            for score, move_x, left, right, center, outer in pipe_scores[:15]
        ],
        "topHighResolutionPipeScores": [
            {
                "moveX": move_x,
                "score": round(score, 4),
                "leftPeak": round(left, 4),
                "rightPeak": round(right, 4),
                "center": round(center, 4),
                "outer": round(outer, 4),
            }
            for score, move_x, left, right, center, outer in high_pipe_scores[:15]
        ],
        "normalProfiles": {
            str(move_x): [round(value, 2) for value in normal_profile(gray, curve_points(coords, move_x))]
            for move_x in (190, 200, 204, 210, 216, 220, 224, 228, 232, 240)
        },
    }, ensure_ascii=True, indent=2))


if __name__ == "__main__":
    main()
