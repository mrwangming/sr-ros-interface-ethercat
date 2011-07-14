#!/usr/bin/env python
#
# Copyright 2011 Shadow Robot Company Ltd.
#
# This program is free software: you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the Free
# Software Foundation, either version 2 of the License, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
# more details.
#
# You should have received a copy of the GNU General Public License along
# with this program.  If not, see <http://www.gnu.org/licenses/>.
#


import roslib; roslib.load_manifest('sr_control_gui')
import rospy

from open_gl_generic_plugin import OpenGLGenericPlugin
from utils.rostopic import RosTopicChecker
from config import Config
import math

from PyQt4 import QtCore, QtGui, Qt
from OpenGL.GL import *
from OpenGL.GLU import *
from OpenGL.GLUT import *
from PyQt4 import QtGui
from PyQt4.QtOpenGL import *

import threading

from collections import deque
from std_msgs.msg import Float64



class DataSet(object):
    default_color = QtCore.Qt.black

    def __init__(self, parent):
        self.parent = parent
        self.subscriber_1 = None
        self.subscriber_2 = None

        self.enabled = False

        self.mutex_1 = threading.Lock()
        self.mutex_2 = threading.Lock()
        self.points_1 = []
        self.points_2 = []
        self.init_dataset()
        self.scaled_color = []
        self.raw_color = []

        self.change_color([255,0,0])

    def init_dataset(self):
        tmp1 = [0]* self.parent.data_points_size
        self.points_1 = deque(tmp1)
        tmp2 = [0]* self.parent.data_points_size
        self.points_2 = deque(tmp2)

    def change_color(self, rgb):
        r = rgb[0]
        g = rgb[1]
        b = rgb[2]

        self.scaled_color = [r, g, b]

        #create a darker version of the raw color
        r *= 0.5
        g *= 0.5
        b *= 0.5
        self.raw_color = [r,g,b]

    def get_raw_color(self):
        return self.raw_color

    def get_scaled_color(self):
        return self.scaled_color

    def callback_1(self, msg):
        """
        Received a message on the first topic. Adding the new point to
        the queue.
        """
        #received message from subscriber at index: update the last
        # point and pop the first point
        #print index, " ", msg.data
        if self.parent.paused:
            return
        #self.mutex_1.acquire()
        self.points_1.append( int(msg.data * 5000) )
        self.points_1.popleft()
        #self.mutex_1.release()

    def callback_2(self, msg):
        """
        Received a message on the first topic. Adding the new point to
        the queue.
        """
        #received message from subscriber at index: update the last
        # point and pop the first point
        #print index, " ", msg.data
        if self.parent.paused:
            return
        #self.mutex_2.acquire()
        self.points_2.append( int(msg.data * 5000) )
        self.points_2.popleft()
        #self.mutex_2.release()

    def close(self):
        self.subscriber_1.unregister()
        self.subscriber_1 = None
        self.subscriber_2.unregister()
        self.subscriber_2 = None


class SubscribeTopicFrame(QtGui.QFrame):
    """
    """

    def __init__(self, parent):
        """
        """
        self.parent = parent

        #contains a data set to store the data
        self.data_set = DataSet(self.parent)

        QtGui.QFrame.__init__(self)
        self.layout = QtGui.QHBoxLayout()

        #add a combo box to select the first topic
        self.topic_box_1 = QtGui.QComboBox(self)
        self.topic_box_1.setToolTip("Choose the first topic you want to plot.")
        self.topic_box_1.addItem("None")
        for pub in parent.all_pubs:
            self.topic_box_1.addItem(pub[0])
        self.connect(self.topic_box_1, QtCore.SIGNAL('activated(int)'), self.onChanged_1)
        self.layout.addWidget(self.topic_box_1)

        #add a combo box to select the second topic against which the first topic is plotted
        self.topic_box_2 = QtGui.QComboBox(self)
        self.topic_box_2.setToolTip("Choose the second topic against which you want to plot the first.")
        self.topic_box_2.addItem("None")
        for pub in parent.all_pubs:
            self.topic_box_2.addItem(pub[0])
        self.connect(self.topic_box_2, QtCore.SIGNAL('activated(int)'), self.onChanged_2)
        self.layout.addWidget(self.topic_box_2)

        #add a combobox to easily change the color
        self.change_color_combobox = QtGui.QComboBox(self)
        self.change_color_combobox.setToolTip("Change the color for this topic")
        self.change_color_combobox.setFixedWidth(45)

        for color in Config.open_gl_generic_plugin_config.colors:
            #the colors are stored as an icon + a color value
            icon = QtGui.QIcon(self.parent.parent.parent.rootPath + '/images/icons/colors/'+color[0] +'.png')
            self.change_color_combobox.addItem( icon, "" )

        #add the color picker at the end of the list for the user to pick a custom color
        icon = QtGui.QIcon(self.parent.parent.parent.rootPath + '/images/icons/color_wheel.png')
        self.change_color_combobox.addItem( icon, "" )

        self.connect(self.change_color_combobox, QtCore.SIGNAL('activated(int)'), self.change_color_clicked)
        self.layout.addWidget(self.change_color_combobox)

        #add a button to add a subscribe topic frame
        self.add_subscribe_topic_btn = QtGui.QPushButton()
        self.add_subscribe_topic_btn.setText('+')
        self.add_subscribe_topic_btn.setToolTip("Add a new topic to plot.")
        self.add_subscribe_topic_btn.setFixedWidth(30)
        self.connect(self.add_subscribe_topic_btn, QtCore.SIGNAL('clicked()'),self.add_subscribe_topic_clicked)
        self.layout.addWidget(self.add_subscribe_topic_btn)

        #add a button to remove the current subscribe topic frame
        if len(self.parent.subscribe_topic_frames) > 0:
            self.remove_topic_btn = QtGui.QPushButton()
            self.remove_topic_btn.setText('-')
            self.remove_topic_btn.setToolTip("Remove this topic.")
            self.remove_topic_btn.setFixedWidth(30)
            self.connect(self.remove_topic_btn, QtCore.SIGNAL('clicked()'),self.remove_topic_clicked)
            self.layout.addWidget(self.remove_topic_btn)

        self.display_last_value = QtGui.QLabel()
        self.display_last_value.setFixedWidth(110)
        self.layout.addWidget(self.display_last_value)

        #set a color by default
        self.change_color_clicked(index = 0, const_color = [255,0,0])

        self.setLayout(self.layout)

    def refresh_topics(self):
        self.topic_box_1.clear()
        self.topic_box_2.clear()
        self.topic_box_1.addItem("None")
        self.topic_box_2.addItem("None")
        for pub in self.parent.all_pubs:
            self.topic_box_1.addItem(pub[0])
            self.topic_box_2.addItem(pub[0])

    def onChanged_1(self, index):
        text = self.topic_box_1.currentText()
        #unsubscribe the current topic
        if self.data_set.subscriber_1 != None:
            self.data_set.subscriber_1.unregister()

        if text != "None":
            self.data_set.enabled = True
            self.data_set.subscriber_1 = rospy.Subscriber(str(text), Float64, self.data_set.callback_1)
        else:
            self.data_set.enabled = False

    def onChanged_2(self, index):
        text = self.topic_box_2.currentText()
        #unsubscribe the current topic
        if self.data_set.subscriber_2 != None:
            self.data_set.subscriber_2.unregister()

        if text != "None":
            self.data_set.enabled = True
            self.data_set.subscriber_2 = rospy.Subscriber(str(text), Float64, self.data_set.callback_2)
        else:
            self.data_set.enabled = False

    def update_display_last_value(self, value):
        if value != None:
            txt = ""
            txt += str(value)
            txt += " / "
            txt += "0x%0.4X" % value
            self.display_last_value.setText(txt)


    def change_color_clicked(self, index, const_color = None):
        # the last item is the color picker
        if index == self.change_color_combobox.count() - 1:
            col_tmp = QtGui.QColorDialog.getColor()
            if not col_tmp.isValid():
                return
            col = [col_tmp.redF(), col_tmp.greenF(), col_tmp.blueF()]
        else:
            #the user choosed a color from the dropdown
            if const_color == None:
                col = Config.open_gl_generic_plugin_config.colors[index][1]
            else:
                col = const_color

        self.data_set.change_color(col)


    def add_subscribe_topic_clicked(self):
        self.parent.add_topic_subscriber()
        Qt.QTimer.singleShot(0, self.adjustSize)

    def remove_topic_clicked(self):
        if self.data_set.subscriber != None:
            self.data_set.subscriber.unregister()
            self.parent.subscriber = None
        self.parent.subscribe_topic_frames.remove(self)

        self.topic_box_1.setParent(None)
        self.topic_box_2.setParent(None)
        self.change_color_btn.setParent(None)
        self.add_subscribe_topic_btn.setParent(None)
        self.remove_topic_btn.setParent(None)
        self.display_last_value.setParent(None)

        #refresh the indexes
        for i,sub_frame in enumerate(self.parent.subscribe_topic_frames):
            sub_frame.subscriber_index = i

        Qt.QTimer.singleShot(0, self.adjustSize)
        del self
    
    def close(self):
        self.data_set.close()

class CircleScope(OpenGLGenericPlugin):
    """
    Plots some chosen debug values.
    """
    name = "Circle Scope"
    number_of_points_to_display = 1000

    def __init__(self):
        OpenGLGenericPlugin.__init__(self, self.paint_method, self.right_click_method)
        self.data_points_size = self.open_gl_widget.number_of_points
        self.all_pubs = []
        self.topic_checker = RosTopicChecker()
        self.refresh_topics()

        # this is used to go back in time
        self.display_frame = 0

        self.mutex = threading.Lock()

        self.control_layout = QtGui.QVBoxLayout()

        self.btn_frame = QtGui.QFrame()
        self.btn_frame_layout = QtGui.QHBoxLayout()
        # add a button to play/pause the display
        self.play_btn = QtGui.QPushButton()
        self.play_btn.setFixedWidth(30)
        self.play_btn.setToolTip("Play/Pause the plots")
        self.btn_frame.connect(self.play_btn, QtCore.SIGNAL('clicked()'), self.button_play_clicked)
        self.btn_frame_layout.addWidget(self.play_btn)

        #add a button to refresh the topics
        self.refresh_btn = QtGui.QPushButton()
        self.refresh_btn.setFixedWidth(30)
        self.refresh_btn.setToolTip("Refresh the list of topics")
        self.btn_frame.connect(self.refresh_btn, QtCore.SIGNAL('clicked()'), self.button_refresh_clicked)
        self.btn_frame_layout.addWidget(self.refresh_btn)

        self.btn_frame.setFixedWidth(80)
        self.btn_frame.setFixedHeight(50)
        self.btn_frame.setLayout(self.btn_frame_layout)

        self.control_layout.addWidget(self.btn_frame)
        self.subscribe_topic_frames = []

        self.time_slider = QtGui.QSlider(QtCore.Qt.Horizontal)
        self.time_slider.setMinimum(0)
        self.time_slider.setMaximum(self.data_points_size - self.number_of_points_to_display)
        self.time_slider.setInvertedControls(True)
        self.time_slider.setInvertedAppearance(True)
        self.time_slider.setEnabled(False)
        self.frame.connect(self.time_slider, QtCore.SIGNAL('valueChanged(int)'), self.time_changed)
        self.layout.addWidget(self.time_slider)

        self.paused = False

    def resized(self):
        #we divide by 100 because we want to scroll 100 points per 100 points
        self.time_slider.setMaximum((self.data_points_size - self.number_of_points_to_display)/100 - 1)

    def activate(self):
        OpenGLGenericPlugin.activate(self)
        self.play_btn.setIcon(QtGui.QIcon(self.parent.parent.rootPath + '/images/icons/pause.png'))
        self.refresh_btn.setIcon(QtGui.QIcon(self.parent.parent.rootPath + '/images/icons/refresh.png'))

        self.add_topic_subscriber()
        self.control_frame.setLayout(self.control_layout)

    def add_topic_subscriber(self):
        index = len(self.subscribe_topic_frames)
        tmp_stf = SubscribeTopicFrame(self)
        self.control_layout.addWidget( tmp_stf )
        self.subscribe_topic_frames.append( tmp_stf )

    def right_click_method(self, x):
        #right click for time traveling
        # only if paused
        if self.paused:
            self.display_frame += (x - self.open_gl_widget.last_right_click_x)
            if self.display_frame < 0:
                self.display_frame = 0
            if self.display_frame >= self.data_points_size:
                self.display_frame = self.data_points_size - 1

            self.open_gl_widget.last_right_click_x = x

            self.paint_method(self.display_frame)

    def paint_method(self, display_frame = 0):
        '''
        Drawing routine: this function is called periodically.

        @display_frame: the frame to display: we have more points than we display.
                        By default, we display the latest values, but by playing
                        with the time_slider we can then go back in time.
        '''
        if self.paused:
            display_frame = self.display_frame

        display_points = []
        colors = []

        for sub_frame in self.subscribe_topic_frames:  
            #sub_frame.data_set.mutex_1.acquire()
            #sub_frame.data_set.mutex_2.acquire()
            for display_index in range(0, self.number_of_points_to_display):
                data_index = self.display_to_data_index(display_index, display_frame)
                #if sub_frame.data_set.points_1[data_index] != None:
                #    if sub_frame.data_set.points_2[data_index] != None:
                # add the raw data
                colors.append(sub_frame.data_set.get_scaled_color())
                #print "point [",data_index,"]: ", sub_frame.data_set.points_2[data_index] , " ",  sub_frame.data_set.points_1[data_index]," ",
                display_points.append([sub_frame.data_set.points_2[data_index] + 300, sub_frame.data_set.points_1[data_index] + 300])
            #sub_frame.data_set.mutex_1.release()
            #sub_frame.data_set.mutex_2.release()
        #glDrawArrays(GL_LINE_STRIP, 0, len(display_points))
        #glPointSize(10)
        #glEnable(GL_POINT_SMOOTH)
        #glEnable(GL_BLEND)
        glEnableClientState(GL_VERTEX_ARRAY)
        glEnableClientState(GL_COLOR_ARRAY)
        glClear(GL_COLOR_BUFFER_BIT)
        glColorPointerf(colors)
        glVertexPointerf(display_points)
        glDrawArrays(GL_POINTS, 0, len(display_points))
        glDisableClientState(GL_VERTEX_ARRAY)
        glDisableClientState(GL_COLOR_ARRAY)
        glFlush()
        self.open_gl_widget.update()

    def time_changed(self, value):
        self.display_frame = value*100
        self.paint_method(value)

    def button_play_clicked(self):
        #lock mutex here
        if not self.paused:
            self.paused = True
            self.play_btn.setIcon(QtGui.QIcon(self.parent.parent.rootPath + '/images/icons/play.png'))
            self.time_slider.setEnabled(True)
        else:
            self.paused = False
            self.play_btn.setIcon(QtGui.QIcon(self.parent.parent.rootPath + '/images/icons/pause.png'))
            self.time_slider.setEnabled(False)

    def button_refresh_clicked(self):
        self.refresh_topics()
        for sub_frame in self.subscribe_topic_frames:
            sub_frame.refresh_topics()

    def refresh_topics(self):
        self.all_pubs = []
        tmp = self.topic_checker.get_topics(publishers_only = True, topic_filter="std_msgs/Float64")
        for sub in tmp:
            topic = sub[0]
            if "position" in topic:
                print topic
                self.all_pubs.append(sub)

    def display_to_data_index(self, display_index, display_frame):
        # we multiply display_frame by a 100 because we're scrolling 100 points per 100 points
        data_index = self.data_points_size - self.number_of_points_to_display + display_index - (display_frame)
        return data_index

    def scale_data(self, data, data_max = 65536):
        scaled_data = (data * self.open_gl_widget.height) / data_max
        return scaled_data

    def on_close(self):
        for sub_frame in self.subscribe_topic_frames:  
            sub_frame.close()
        OpenGLGenericPlugin.on_close(self)
