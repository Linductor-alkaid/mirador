"""Offline UI checkpoint -> ONNX export and preprocessing comparison.
Uses explicit local files; no downloading. Requires CPU torch/torchvision,
Ultralytics 8.3.0, opencv, Pillow, onnxruntime and numpy.
"""
import argparse
import json
from pathlib import Path

import cv2
import numpy as np
import onnxruntime as ort
import torch
from PIL import Image
from ultralytics import YOLO

from review_real_results import decode, match


def rows(raw):
    values = raw[0].T
    return np.concatenate([values[:, :4], np.ones((len(values), 1), np.float32), values[:, 4:]], axis=1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['checkpoint', 'onnx', 'image', 'output']:
        parser.add_argument('--'+name, required=True, type=Path)
    args = parser.parse_args()
    torch.set_num_threads(1)
    torch.set_num_interop_threads(1)
    model = YOLO(str(args.checkpoint))
    original = Image.open(args.image).convert('RGB')
    pixels = np.asarray(original)
    height, width = pixels.shape[:2]
    scale = min(640/width, 640/height)
    rw, rh = round(width*scale), round(height*scale)
    canvas = np.full((640, 640, 3), 114, np.uint8)
    px, py = (640-rw)//2, (640-rh)//2
    canvas[py:py+rh, px:px+rw] = cv2.resize(pixels, (rw,rh), interpolation=cv2.INTER_LINEAR)
    input_tensor = canvas.transpose(2,0,1)[None].astype(np.float32)/255
    with torch.inference_mode():
        torch_raw = model.model.float().eval()(torch.from_numpy(input_tensor))[0].numpy()
    options = ort.SessionOptions()
    options.intra_op_num_threads = 1
    options.inter_op_num_threads = 1
    session = ort.InferenceSession(str(args.onnx), options, providers=['CPUExecutionProvider'])
    onnx_raw = session.run(None, {session.get_inputs()[0].name: input_tensor})[0]
    torch_boxes = decode(rows(torch_raw), width, height, [rw,rh], .25)
    onnx_boxes = decode(rows(onnx_raw), width, height, [rw,rh], .25)
    matched, minimum_iou = match(torch_boxes, onnx_boxes)
    native = model.predict(source=original, imgsz=640, conf=.25, iou=.5, device='cpu', verbose=False)[0]
    native_boxes = [{'class':int(c), 'score':float(s), 'bounds':[x,y,x1-x,y1-y]}
                    for (x,y,x1,y1),c,s in zip(native.boxes.xyxy.tolist(),native.boxes.cls.tolist(),native.boxes.conf.tolist())]
    result = dict(fixed_shape=list(input_tensor.shape), raw_abs_max=float(np.abs(torch_raw-onnx_raw).max()),
                  matched=matched, minimum_iou=minimum_iou, torch_fixed=torch_boxes, onnx_fixed=onnx_boxes,
                  native_auto_shape=list(model.predictor.preprocess([pixels[:,:,::-1].copy()]).shape),
                  native_auto=native_boxes)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(result,indent=2)+'\n')
    if matched != len(torch_boxes) or matched != len(onnx_boxes):
        raise SystemExit('checkpoint -> ONNX comparison failed')


if __name__ == '__main__':
    main()
