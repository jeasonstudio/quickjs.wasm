import { name } from './constant.js';
import * as std from 'std';
import * as os from 'os';

console.log(`Hello ${name ?? 'World'}!`);
console.log(Object.keys(std));
console.log(Object.keys(os));

std.loadFile('./constant.js', console.log);
