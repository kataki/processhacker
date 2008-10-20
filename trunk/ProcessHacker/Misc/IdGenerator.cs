﻿/*
 * Process Hacker
 * 
 * Copyright (C) 2008 wj32
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

using System;
using System.Collections.Generic;
using System.Text;

namespace ProcessHacker
{
    public class IdGenerator
    {
        List<int> _ids = new List<int>();
        int _id = 0;

        public int Pop()
        {
            if (_ids.Count > 0)
            {
                int id = _ids[0];

                _ids.Remove(_ids[0]);

                return id;
            }

            return _id++;
        }

        public void Push(int id)
        {
            _ids.Add(id);
            _ids.Sort();
        }
    }
}
