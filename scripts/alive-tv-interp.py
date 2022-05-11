#!/usr/bin/python3

import argparse
import json
from json.decoder import JSONDecodeError
import os
import string
import subprocess
import yaml
import sys

class IrTag(yaml.YAMLObject):
    yaml_tag = '!ir'

    def __init__(self, ir_text):
        self.ir_text = ir_text

    def __repr__(self):
        return 'IrTag(\n{})'.format(self.ir_text)

    @classmethod
    def from_yaml(cls, loader, node):
        return IrTag(node.value)

    @classmethod
    def to_yaml(cls, dumper, data):
        if len(data.ir_text.splitlines()) > 1:
            return dumper.represent_scalar(cls.yaml_tag, data.ir_text, style='|')
        return dumper.represent_scalar(cls.yaml_tag, data.ir_text)


def run_command(command_list, echo=True, timout=10):

    if echo:
        print("executing:" + ' '.join(command_list))
    try:
        pr = subprocess.run(command_list, capture_output=True,
                            check=True, timeout=timout)
    except subprocess.CalledProcessError as e:
        #print(
        #    f'run_command {" ".join(e.cmd)} returned with non_zero exit code')
        #print(e.output)
        return e.returncode, e.stdout, e.stderr, False
    except subprocess.TimeoutExpired as e:
        #print(f'run_command {" ".join(e.cmd)} timed out')
        return e.timeout, e.stdout, e.stderr, True

    return pr.returncode, pr.stdout, pr.stderr, False


def run_alive_worker(yaml_path: str, alive_worker_path : str, timeout: int = 10):
    
    return_code, stdout, stderr, timed_out = run_command([alive_worker_path, yaml_path], echo=False, timout=timeout)
    if timed_out:
        return None, None, timed_out
    #elif return_code != 0:
    #    print(f'alive-tv returned with non_zero exit code')
    #    #print(stderr)
    #    return stderr, timed_out
    #else:
    #    print(f'alive-tv returned with zero exit code')
    #    print(stderr)
    #    return stdout, timed_out
    return stdout, stderr, timed_out


def run_directory_w_alive_worker(src_dir: str, alive_worker_path: str, timeout: int = 10):
    #print(f'run_directory_w_alive_worker src_dir={src_dir}, alive_worker_path={alive_worker_path}')
    files = os.listdir(src_dir)
    dir_pass_count, dir_fail_count, dir_timeout_count = 0, 0, 0
    fn_pass_count, fn_fail_count, timed_out_count = 0, 0, 0
    failed_tests = []
    timed_out_tests = []
    for src_file_dir in files:
        f_path = os.path.join(src_dir, src_file_dir)
        if os.path.isdir(f_path):
            sub_fn_pass_count, sub_fn_fail_count, sub_timed_out_count = run_directory_w_alive_worker(
                f_path, alive_worker_path, timeout)
            dir_pass_count+=sub_fn_pass_count
            dir_fail_count+=sub_fn_fail_count
            dir_timeout_count+=sub_timed_out_count
            continue
        alive_stdout, alive_stderr, timed_out = run_alive_worker(
            f_path, alive_worker_path, timeout)
        failed = False
        if not timed_out:
            #print(f'alive_stdout:\n{alive_stdout}-----')
            #print(f'alive_stderr:\n{alive_stderr}-----')
            for line in alive_stderr.splitlines():
                if line.endswith('passed'):
                    fn_pass_count += 1
                elif line.endswith('failed'):
                    fn_fail_count += 1
                    failed = True
            if failed:
                failed_tests.append(src_file_dir)
        else:
            timed_out_count += 1
            timed_out_tests.append(src_file_dir)
    print(f'---{src_dir} test exec result---')
    print(f'# functions passed: {fn_pass_count}')
    print(f'# functions failed: {fn_fail_count}')
    print(f'# tests timed out: {timed_out_count}')
    dir_pass_count+=fn_pass_count
    dir_fail_count+=fn_fail_count
    dir_timeout_count+=timed_out_count
    if len(failed_tests) > 0:
        print('---failed tests---')
        for failed_test in failed_tests:
            print(failed_test)
    if timed_out_count:
        print('---timed out tests---')
        for timed_out_test in timed_out_tests:
            print(timed_out_test)
    # , failed_tests, timed_out_tests
    return dir_pass_count, dir_fail_count, dir_timeout_count


def run_directory_w_alive_tv(src_dir: str, alive_tv_path: str, timeout: int = 10):
    #print(f'run_directory_w_alive_tv src_dir={src_dir}, alive_tv_path={alive_tv_path}')
    files = os.listdir(src_dir)
    dir_return_count, dir_timeout_count = 0, 0
    test_return_count, timed_out_count = 0, 0
    timed_out_tests = []
    for src_file_dir in files:
        f_path = os.path.join(src_dir, src_file_dir)
        if os.path.isdir(f_path):
            sub_timed_out_count, sub_return_count = run_directory_w_alive_tv(
                f_path, alive_tv_path, timeout)
            dir_return_count+=sub_return_count
            dir_timeout_count+=sub_timed_out_count
            continue
        _, _, _, timed_out = run_command([alive_tv_path, f_path], echo=False, timout=timeout)
        if timed_out:
            timed_out_count += 1
            timed_out_tests.append(src_file_dir)
        else: 
            test_return_count += 1
        
    print(f'---{src_dir} alive-tv timout result---')
    print(f'# tests returned: {test_return_count}')
    print(f'# tests timed out: {timed_out_count}')
    dir_return_count+=test_return_count
    dir_timeout_count+=timed_out_count
    
    if timed_out_count:
        print('---timed out tests---')
        for timed_out_test in timed_out_tests:
            print(timed_out_test)
    # , failed_tests, timed_out_tests
    return dir_timeout_count, dir_return_count

# read an .ll file and return a list of IrTag objects
def read_ir_file(file_name: str):
    fns = []
    decls = []
    with open(file_name, 'r') as in_stream:
        in_function = False
        for o_line in in_stream:
            line = o_line.strip()
            if line.startswith('define'):
                assert(in_function == False)
                in_function = True
                br_count = 0
                fns.append([])
            if in_function:
                fns[-1].append(o_line)
                br_count += line.count('{')
                br_count -= line.count('}')
                if br_count == 0:
                    in_function = False
            else:
                decls.append(o_line)

    decls_str = ''.join(decls)
    ir_tags = []
    for fn_lines in fns:
        fn_str = ''.join(fn_lines)
        ir_tags.append(IrTag(decls_str + fn_str))
        
    # print(ir_tags)
    return ir_tags

# yaml reading and writing
def get_loader():
    loader = yaml.SafeLoader
    loader.add_constructor("!ir", IrTag.from_yaml)
    return loader

def read_alive_yaml(input_path: str):
    with open(input_path, 'r') as file:
        y_obj = yaml.load(file, Loader=get_loader())
        return y_obj
    return None

def generate_yaml(o_path: str, name: str, func: str, args: list, ir_tags: list = None, expected: dict = None):
    #print(f'generate yaml o_path={o_path}')
    y_obj = dict()
    y_obj['name'] = name
    y_obj['func'] = func
    y_obj['args'] = args
    y_obj['expected'] = expected
    y_obj['args'].extend(ir_tags)
    with open(o_path, 'w') as o_file:
        yaml.dump(y_obj, o_file)

# return number of functions in the alive-interp yaml test file
def generate_alive_interp_yaml(o_path: str, tv_yaml_obj, test_input: dict):
    # print(f'generate alive-interp yaml o_path={o_path}')
    ir_tags = []
    fn_cnt = 0
    if tv_yaml_obj: 
        for tv_arg in tv_yaml_obj['args']:
            if isinstance(tv_arg, IrTag):
                ir_tags.append(tv_arg)
                fn_cnt+=1
    
    if (fn_cnt):
        tgt_base, tgt_file = os.path.split(o_path)
        tgt_prefix = tgt_file[:tgt_file.index('.interp')]
        y_objs = []
        with open(o_path, 'w') as o_file:
            for i in range(fn_cnt):
                y_obj = dict()
                y_obj['name'] = tgt_prefix + '-' + str(i)
                y_obj['func'] = 'alive.interpret'
                y_obj['expected'] = {'status' : 'done'}
                y_obj['args'] = [{}]
                y_obj['args'].append(ir_tags[i])
                y_obj['args'].append(test_input)
                y_objs.append(y_obj)
                
            yaml.dump_all(y_objs, o_file)
        return fn_cnt
    return 0

# given a .ll file, convert to alive-tv yaml
def convert_test_to_alive_tv_yaml(src_path: str, tgt_dir: str):
    src_base, src_file = os.path.split(src_path)
    # print(src_file)
    if src_file.endswith(".srctgt.ll"):
        src_prefix = src_file.index(".ll")
        tgt_file = src_file[:src_prefix] + ".alive-tv.yaml"
        tgt_path = os.path.join(tgt_dir, tgt_file)
        # print(tgt_file)
        # print(tgt_path)
        ir_tags = read_ir_file(src_path)
        if (len(ir_tags) != 2):
            print(f'{src_path} has {len(ir_tags)} ir_tags, expected 2')
            return 0
        #assert(len(ir_tags) == 2)# must have 2 functions src and tgt
        # print(ir_tags)
        generate_yaml(tgt_path, tgt_file, 'alive.tv_v2',
                      [{'disable_undef_input': False, 'disable_poison_input': False}], ir_tags)
        return 2
    elif src_file.endswith(".ll"):
        src_prefix = src_file.index(".ll")
        tgt_file = src_file[:src_prefix] + ".alive-tv.yaml"
        tgt_path = os.path.join(tgt_dir, tgt_file)
        ir_tags = read_ir_file(src_path)
        if (len(ir_tags) == 0):
            return 0
        generate_yaml(tgt_path, tgt_file, 'alive.tv_v2',
                      [{'disable_undef_input': False, 'disable_poison_input': False}], ir_tags)
        return len(ir_tags)
    else:
        return 0

def convert_tv_to_alive_interp(src_path: str, tgt_dir: str, alive_worker_path: str):
    #print(f'convert_tv_to_alive_interp {src_path}, {tgt_dir}, {alive_worker_path}')
    src_base, src_file = os.path.split(src_path)
    #run alive-worker-test on tgt_path
    alive_stdout, alive_std_err, timed_out = run_alive_worker(src_path, alive_worker_path, 100) 
    if not alive_stdout:
        alive_stdout = ''
    if not alive_std_err:
        alive_std_err = ''
    alive_lines = alive_stdout + '\n' + alive_std_err
    if timed_out:
        print(f'alive-tv timed out')
        return 0
    else:
        #print(f'alive-tv returned')
        #print(alive_lines)
        lines = alive_lines.splitlines()
        for line in lines:
            if line.startswith('actual:'):
                try:
                    j_obj = json.loads(line[8:])
                    #print(j_obj)
                    if 'test_input' in j_obj: #only convert test cases with counter examples
                        test_input_dict = j_obj['test_input']
                        #print(test_input_dict)
                        y_obj = read_alive_yaml(src_path)
                        assert(src_file.endswith(".alive-tv.yaml"))
                        src_prefix = src_file.index(".alive-tv.yaml")
                        tgt_file = src_file[:src_prefix] + ".interp.yaml"
                        tgt_path = os.path.join(tgt_dir, tgt_file)
                        converted_functions = generate_alive_interp_yaml(tgt_path, y_obj, test_input_dict)
                        return converted_functions
                except JSONDecodeError as e:
                    print(f'source file {src_path}')
                    print(f'JSONDecodeError: {e}')
                    return 0
                    
    return 0

# convert .ll files in a directory to alive-worker-test yaml files
def convert_directory_to_alive_worker(src_dir: str, tgt_dir: str, executable_path: str=None, mode:str='tv'):
    print(f'convert_directory_to_alive_worker {src_dir}, {tgt_dir}, {executable_path}, {mode}')
    # delete tgt_dir contents
    run_command(['rm', '-rf', tgt_dir])

    try:
        os.mkdir(tgt_dir)
    except OSError as e:
        print(e)
        exit(1)

    list_dir_files = os.listdir(src_dir)
    src_file_cnt, tgt_file_cnt = 0, 0
    src_fn_cnt, tgt_fn_cnt = 0, 0
    for src_file_dir in list_dir_files:
        f_path = os.path.join(src_dir, src_file_dir)
        if os.path.isdir(f_path):  
            _ , src_c_dir = os.path.split(f_path)
            #print(f'Skipping directory {src_base}, {src_c_dir}')
            c_tgt_dir = os.path.join(tgt_dir, src_c_dir)
            #print(f'tgt directory {c_tgt_dir}')
            convert_directory_to_alive_worker(f_path, c_tgt_dir, executable_path, mode)
            continue
        src_file_cnt+=1
        if mode == 'tv':
            #print(f"converting to alive-tv yaml: {f_path}")
            fns_converted = convert_test_to_alive_tv_yaml(f_path, tgt_dir)
            if fns_converted:
                tgt_file_cnt+=1
                tgt_fn_cnt+=fns_converted
        elif mode == 'interp':
            fns_converted = convert_tv_to_alive_interp(f_path, tgt_dir, executable_path)
            if fns_converted:
                tgt_file_cnt+=1
                tgt_fn_cnt+=fns_converted
        else:
            print(f"convert_directory_to_alive_worker: unsupported mode {mode}")
            exit(1)

    list_dir = os.listdir(tgt_dir)
    print('---conversion result---')
    if mode == 'tv':
        print(f'# llvm-ir files : {src_file_cnt}')
        print(f'# alive-tv yaml files : {tgt_file_cnt}')
        print(f'# alive-tv functions : {tgt_fn_cnt}')
    elif mode == 'interp':
        print(f'# alive-tv yaml files : {src_file_cnt}')
        print(f'# alive-interp yaml files: {tgt_file_cnt}')
        print(f'# alive-interp functions: {tgt_fn_cnt}')
    return list_dir

 
if __name__ == "__main__":
    #ir_tags = read_ir_file('/home/nader/code/nader_alive2/tests/alive-tv/bugs/pr32872.srctgt.ll')
    #print(ir_tags)
    #exit()
    parser = argparse.ArgumentParser(
        description='Convert llvm-ir tests to format compatible with alive-worker-test')
    parser.add_argument('-ct', '--convert-tv', dest='convert_tv',
                        help='convert contents of <src_dir> to alive-tv yaml and store in <tgt_dir>', default=None, nargs='+')
    parser.add_argument('-ci', '--convert-interp', dest='convert_interp',
                        help='convert contents of <src_dir> alive-tv yaml and store in <tgt_dir> with alive-interp yaml files using <alive-worker-test> path',
                        default=None, nargs='+')
    parser.add_argument('-e', '--exec', dest='worker_exec',
                        help='run <alive-worker-test> on contents of <input_dir>', default=None, nargs='+')
    parser.add_argument('-tv', '--tvalidation', dest='tv_args',
                        help='run <alive-tv> on contents of <input_dir> and report timeouts', default=None, nargs='+')
    opts = parser.parse_args()

    if opts.convert_tv and opts.convert_interp:
        print('cannot convert tv and interp at the same time')
        exit(1)
    
    if opts.convert_tv: 
        assert(len(opts.convert_tv) == 2 and '-ct requires a <src_dir> and <tgt_dir>' )
        convert_directory_to_alive_worker(*opts.convert_tv, mode='tv')
    
    if opts.convert_interp:
        convert_directory_to_alive_worker(*opts.convert_interp, mode='interp')

    if opts.worker_exec:
        print(opts.worker_exec)
        tests_passed, tests_fail, tests_timout = run_directory_w_alive_worker(opts.worker_exec[1], opts.worker_exec[0])
        print(f'---aggregate result---')
        print(f'# functions passed : {tests_passed}')
        print(f'# functions failed : {tests_fail}')
        print(f'# tests timedout : {tests_timout}')
        print(f'# % functions passed: {tests_passed/(tests_passed+tests_fail+tests_timout)*100:.2f}')

    if opts.tv_args:
        print(opts.tv_args)
        assert(opts.tv_args[2].isnumeric() and 'alive-tv timout should be an integer value')
        tests_timout, tests_return = run_directory_w_alive_tv(opts.tv_args[1], opts.tv_args[0], int(opts.tv_args[2]) )
        print(f'---aggregate result---')
        print(f'# tests returned : {tests_return}')
        print(f'# tests timed out : {tests_timout}')
        print(f'# % functions returned : {tests_return/(tests_timout+tests_return)*100:.2f}')
