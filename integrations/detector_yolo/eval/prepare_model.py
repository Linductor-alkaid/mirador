"""Prepare a caller-supplied ONNX model for the frozen Mirador YOLO row contract.
No network or package installation. Requires onnx and numpy. Converter must be
pnnx 20260526, validated with the pinned ncnn 20260526 runtime.
"""
import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

# SHA-256 of pnnx from the official pnnx-20260526-linux.zip, checked below.
CONVERTER_SHA256 = 'c3555c48e054245e50fa434a7673fbdad2b8f8c6cdca8c1579601f9882f474a0'


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def promote_fp32(model):
    def promote(tensor):
        if tensor.data_type == TensorProto.FLOAT16:
            tensor.CopyFrom(numpy_helper.from_array(numpy_helper.to_array(tensor).astype(np.float32), tensor.name))
    for tensor in model.graph.initializer:
        promote(tensor)
    for node in model.graph.node:
        for attr in node.attribute:
            if attr.type == onnx.AttributeProto.TENSOR:
                promote(attr.t)
            if node.op_type == 'Cast' and attr.name == 'to' and attr.i == TensorProto.FLOAT16:
                attr.i = TensorProto.FLOAT
    for value in list(model.graph.input)+list(model.graph.output)+list(model.graph.value_info):
        if value.type.tensor_type.elem_type == TensorProto.FLOAT16:
            value.type.tensor_type.elem_type = TensorProto.FLOAT


def normalize_class_only(model, classes):
    output = model.graph.output[0]
    shape = [d.dim_value for d in output.type.tensor_type.shape.dim]
    if len(shape) != 3 or shape[0] != 1 or shape[1] != 4+classes or not 1 <= shape[2] <= 100000:
        raise ValueError('expected static [1,4+C,N] class-only output')
    # These reserved names may not collide with an existing graph.
    names = {name for node in model.graph.node for name in list(node.input)+list(node.output)}
    if any(name.startswith('mirador_eval_') for name in names):
        raise ValueError('reserved normalization prefix already in graph')
    old = output.name
    for node in model.graph.node:
        for i, name in enumerate(node.output):
            if name == old:
                node.output[i] = 'mirador_eval_decoded'
    model.graph.node.append(helper.make_node('Transpose',['mirador_eval_decoded'],['mirador_eval_rows'],perm=[0,2,1]))
    arrays = {'starts':np.array([0],np.int64),'ends':np.array([4],np.int64),'axes':np.array([2],np.int64),
              'score_start':np.array([4],np.int64),'score_end':np.array([4+classes],np.int64),
              'objectness':np.ones((1,shape[2],1),np.float32)}
    for name,array in arrays.items():
        model.graph.initializer.append(numpy_helper.from_array(array,'mirador_eval_'+name))
    model.graph.node.extend([
        helper.make_node('Slice',['mirador_eval_rows','mirador_eval_starts','mirador_eval_ends','mirador_eval_axes'],['mirador_eval_xywh']),
        helper.make_node('Slice',['mirador_eval_rows','mirador_eval_score_start','mirador_eval_score_end','mirador_eval_axes'],['mirador_eval_score']),
        helper.make_node('Concat',['mirador_eval_xywh','mirador_eval_objectness','mirador_eval_score'],['mirador_eval_out'],axis=2)])
    del model.graph.output[:]
    model.graph.output.append(helper.make_tensor_value_info('mirador_eval_out',TensorProto.FLOAT,[1,shape[2],5+classes]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--onnx',required=True,type=Path)
    parser.add_argument('--pnnx',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--layout',choices=['rows','xywh_class'],required=True)
    parser.add_argument('--classes',required=True,type=int)
    args = parser.parse_args()
    if sha256(args.pnnx) != CONVERTER_SHA256:
        raise ValueError('unvalidated converter; use the pinned official pnnx 20260526 Linux executable')
    if args.classes < 1 or args.classes > 1000:
        raise ValueError('class count out of bound')
    if not args.output.name.replace('_','').isalnum():
        raise ValueError('output stem must use letters, digits or underscores')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    model = onnx.load(str(args.onnx))
    promote_fp32(model)
    if args.layout == 'xywh_class':
        normalize_class_only(model,args.classes)
    else:
        shape=[d.dim_value for d in model.graph.output[0].type.tensor_type.shape.dim]
        if len(shape)!=3 or shape[0]!=1 or shape[2]!=5+args.classes or not 1<=shape[1]<=100000:
            raise ValueError('expected static [1,N,5+C] rows')
    onnx.checker.check_model(model)
    converted = args.output.with_suffix('.onnx')
    if converted.resolve() == args.onnx.resolve():
        raise ValueError('output may not overwrite source model')
    onnx.save(model,str(converted))
    with args.output.with_suffix('.conversion.log').open('w') as log:
        subprocess.run([str(args.pnnx.resolve()),str(converted),'inputshape=[1,3,640,640]','fp16=0'],
                       stdout=log,stderr=log,check=True,timeout=180)
    param = args.output.with_suffix('.ncnn.param')
    tokens=[line.split() for line in param.read_text().splitlines()]
    # Only blob/layer names change; learned weights and operators stay identical.
    args.output.with_suffix('.param').write_text('\n'.join(' '.join({'in0':'x','out0':'out'}.get(t,t) for t in line) for line in tokens)+'\n')
    shutil.copyfile(args.output.with_suffix('.ncnn.bin'),args.output.with_suffix('.bin'))
    record=dict(source_sha256=sha256(args.onnx),pnnx_sha256=sha256(args.pnnx),layout=args.layout,
                classes=args.classes,side=640,fp16_storage=False,
                outputs={kind:sha256(args.output.with_suffix('.'+kind)) for kind in ['onnx','param','bin']})
    args.output.with_suffix('.provenance.json').write_text(json.dumps(record,indent=2))


if __name__ == '__main__':
    main()
