"""Offline real-model review. Requires numpy, Pillow, onnxruntime; never downloads.
Inputs and generated artifacts stay in the caller's explicit directory.
"""
import argparse
import hashlib
import html
import json
import math
import subprocess
from pathlib import Path

import numpy as np
import onnxruntime as ort
from PIL import Image, ImageDraw


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def iou(a, b):
    x0, y0 = max(a[0], b[0]), max(a[1], b[1])
    x1, y1 = min(a[0] + a[2], b[0] + b[2]), min(a[1] + a[3], b[1] + b[3])
    intersection = max(0, x1-x0) * max(0, y1-y0)
    return intersection / max(1e-12, a[2]*a[3] + b[2]*b[3] - intersection)


def decode(rows, width, height, resized, threshold):
    """Independent float32 scoring, inverse executed letterbox, class-aware NMS."""
    candidates = []
    sx, sy = resized[0] / width, resized[1] / height
    px, py = (640-resized[0])//2, (640-resized[1])//2
    for row in rows:
        label = int(np.argmax(row[5:]))
        score = float(np.float32(row[4] * row[5+label]))
        if score < threshold:
            continue
        cx, cy, w, h = map(float, row[:4])
        x0, y0 = max(0, (cx-w/2-px)/sx), max(0, (cy-h/2-py)/sy)
        x1, y1 = min(width, (cx+w/2-px)/sx), min(height, (cy+h/2-py)/sy)
        if x1 <= x0 or y1 <= y0:
            continue
        candidates.append(dict(**{'class': label}, score=score, bounds=[x0,y0,x1-x0,y1-y0]))
    candidates.sort(key=lambda r: -r['score'])
    kept = []
    for candidate in candidates:
        if not any(k['class'] == candidate['class'] and iou(k['bounds'], candidate['bounds']) > .5 for k in kept):
            kept.append(candidate)
    return kept


def match(reference, actual, min_iou=.99, max_score=.001):
    used = set()
    matches = []
    for r in reference:
        pool = [(iou(r['bounds'],a['bounds']),i,a) for i,a in enumerate(actual)
                if i not in used and r['class'] == a['class']]
        if not pool:
            continue
        overlap, index, candidate = max(pool, key=lambda x: x[0])
        if overlap >= min_iou and abs(r['score']-candidate['score']) <= max_score:
            used.add(index)
            matches.append(overlap)
    return len(matches), min(matches, default=1.)


def ground_truth(case, actual):
    if 'targets' not in case:
        return None
    used, hits, details = set(), [], []
    for target in case['targets']:
        pool = [(iou(target['bounds'],a['bounds']),i) for i,a in enumerate(actual)
                if i not in used and a['class'] == target['class']]
        overlap, index = max(pool, default=(0,-1))
        if overlap >= .5:
            used.add(index)
            hits.append(target['name'])
            bounds, gt = actual[index]['bounds'], target['bounds']
            cx, cy = bounds[0]+bounds[2]/2, bounds[1]+bounds[3]/2
            details.append(dict(name=target['name'], iou=overlap,
                                center_error_px=math.hypot(cx-gt[0]-gt[2]/2, cy-gt[1]-gt[3]/2),
                                center_inside_gt=gt[0] <= cx < gt[0]+gt[2] and gt[1] <= cy < gt[1]+gt[3]))
    return dict(targets=len(case['targets']), matched=len(hits), detections=len(actual),
                precision=len(hits)/len(actual) if actual else (1. if not case['targets'] else 0.),
                recall=len(hits)/len(case['targets']) if case['targets'] else 1., matched_names=hits, matched_coordinates=details)


def annotate(path, actual, targets, out):
    image = Image.open(path).convert('RGB')
    draw = ImageDraw.Draw(image)
    for index, target in enumerate(targets):
        x,y,w,h = target['bounds']
        draw.rectangle((x,y,x+w,y+h),outline='#16834a',width=2)
        draw.text((x,max(0,y-14)), 'GT '+target['name'], fill='#16834a')
    for index, r in enumerate(actual):
        x,y,w,h = r['bounds']
        draw.rectangle((x,y,x+w,y+h),outline='#e33263',width=3)
        draw.text((x,y), '{}:c{} {:.2f}'.format(index+1,r['class'],r['score']),fill='#e33263')
    image.save(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', required=True, type=Path)
    parser.add_argument('--executable', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--iterations', type=int, default=11)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text())
    args.output.mkdir(parents=True, exist_ok=True)
    all_rows, sessions = [], {}
    options = ort.SessionOptions()
    options.intra_op_num_threads = 1
    options.inter_op_num_threads = 1
    for name, model in manifest['models'].items():
        for kind in ['param','bin','onnx']:
            if digest(model[kind]) != model['sha256'][kind]:
                raise ValueError('model digest differs: '+name+'/'+kind)
        sessions[name] = ort.InferenceSession(model['onnx'], options, providers=['CPUExecutionProvider'])
    for case in manifest['cases']:
        prefix = args.output / case['id']
        model = manifest['models'][case['model']]
        if digest(case['image']) != case['sha256']:
            raise ValueError('image digest differs: '+case['id'])
        subprocess.run([str(args.executable.resolve()),model['param'],model['bin'],str(model['classes']),
                        case['image'],str(prefix),str(args.iterations),str(case.get('confidence',.25)),
                        str(int(case.get('contract',False)))] +
                       ([model['preprocessing']] if model.get('preprocessing') else []),check=True,timeout=300)
        result = json.loads(prefix.with_suffix('.json').read_text())
        input_tensor = np.fromfile(str(prefix)+'.input.f32',np.float32).reshape(1,3,640,640)
        session = sessions[case['model']]
        output = session.run(None,{session.get_inputs()[0].name: input_tensor})[0]
        if model.get('layout') == 'xywh_class':
            rows = output[0].T
            output = np.concatenate([rows[:,:4],np.ones((len(rows),1),np.float32),rows[:,4:]],axis=1)[None]
        raw = np.fromfile(str(prefix)+'.raw.f32',np.float32).reshape(output.shape)
        differences = np.abs(raw-output)
        reference = decode(output[0],result['width'],result['height'],result['resized'],case.get('confidence',.25))
        matched, worst_iou = match(reference,result['detections'])
        comparison = dict(raw_abs_max=float(differences.max()),raw_abs_p99=float(np.quantile(differences,.99)),
                          reference_count=len(reference),actual_count=len(result['detections']),matched=matched,
                          min_matched_iou=worst_iou,
                          passed=matched==len(reference)==len(result['detections']))
        result.update(id=case['id'],model=case['model'],iterations=args.iterations,source_sha256=case['sha256'],
                      oracle=comparison,ground_truth=ground_truth(case,result['detections']))
        prefix.with_suffix('.json').write_text(json.dumps(result,indent=2))
        annotate(case['image'],result['detections'],case.get('targets',[]),prefix.with_suffix('.png'))
        all_rows.append(result)
        print(case['id'],json.dumps(comparison),flush=True)
    (args.output/'summary.json').write_text(json.dumps(all_rows,indent=2))
    cards=[]
    for row in all_rows:
        cards.append('<section><h2>{}</h2><p>检测 {} 个；ONNX 对照 {}；完整 p50 {:.1f}ms；GT {}</p><img src="{}.png"></section>'.format(
            html.escape(row['id']),len(row['detections']),row['oracle']['passed'],row['full_p50_us']/1000,
            html.escape(json.dumps(row['ground_truth'],ensure_ascii=False)),html.escape(row['id'])))
    (args.output/'review.html').write_text('<!doctype html><meta charset="utf-8"><title>YOLO 真实模型复核</title>'
      '<style>body{font:16px system-ui;max-width:1200px;margin:32px auto;background:#f3f5fa;color:#16233c}'
      'img{max-width:100%;border:1px solid #cbd3df}section{background:white;padding:20px;margin:20px 0}</style>'
      '<h1>Mirador YOLO 真实模型视觉复核</h1><p>粉色：Mirador 检测；绿色：独立人工标注的评测目标。'
      '无 GT 的图片仅作探索性复核，不计 precision/recall；小样本不代表标准数据集 mAP。</p>'+''.join(cards))
    if any(not row['oracle']['passed'] for row in all_rows):
        raise SystemExit('ONNX differential comparison failed; inspect summary.json')


if __name__ == '__main__':
    main()
